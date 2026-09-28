/**
 * @file batch.cppm
 * @brief Sketches packed sequences in Vulkan batches and downloads query results.
 */
module;

#include <libassert/assert.hpp>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.vulkan_minimizers:batch;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.hashers.base;
import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.instrumentation;
import :contract;
import :upload;
import :memory;
import :sketch_dispatch;
import :index_materialization;

namespace bioseq::gpu::vulkan::authorized_readback_detail {

/** Checked record count and byte sizes for a host download buffer. */
struct typed_destination_plan {
    std::size_t host_count = 0u;
    std::uint64_t exact_bytes = 0u;
    std::uint64_t preallocation_bytes = 0u;
};

/** Returns record strage bytes, throwing vulkan_runtime_error on multiplication overflow. */
[[nodiscard]] inline std::uint64_t checked_record_bytes(
    std::uint64_t count,
    std::uint64_t record_bytes) {
    const auto bytes = bioseq::common::checked_mul(count, record_bytes);
    if (!bytes.has_value()) {
        throw vulkan_runtime_error{"authorized typed query readback byte extent overflowed"};
    }
    return *bytes;
}

/**
 * @brief Checks that a host record buffer can receive  the permitted device output
 *
 * The C++ record size must match the device format. The requested bytes must
 * match, the readback permission and fit both the device buffer and host limits.
 * @throws vulkan_runtime_error If any count, size, or capacity check fails.
 */
template <class p_minimizer_type>
[[nodiscard]] typed_destination_plan make_typed_destination_plan(
    std::uint64_t host_count,
    std::uint64_t wire_record_bytes,
    std::uint64_t authorization_bytes,
    std::uint64_t device_buffer_bytes,
    std::uint64_t device_allocation_bytes,
    std::size_t maximum_host_count) {
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    if (sizeof(p_minimizer_type) != wire_record_bytes ||
        host_count > (std::numeric_limits<std::size_t>::max)()) {
        throw vulkan_runtime_error{"authorized typed query readback count or record shape is invalid"};
    }
    const std::uint64_t typed_bytes = checked_record_bytes(
        host_count, sizeof(p_minimizer_type));
    const std::uint64_t receipt_bytes = checked_record_bytes(
        host_count, wire_record_bytes);
    if (typed_bytes > (std::numeric_limits<std::size_t>::max)() ||
        typed_bytes != receipt_bytes || typed_bytes != authorization_bytes ||
        typed_bytes > device_buffer_bytes || typed_bytes > device_allocation_bytes) {
        throw vulkan_runtime_error{"authorized typed query readback byte extent is not exact"};
    }
    const auto preallocation_bytes = bioseq::common::checked_mul(
        typed_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (!preallocation_bytes.has_value() || host_count > maximum_host_count) {
        throw vulkan_runtime_error{"authorized typed query capacity is not representable"};
    }
    return typed_destination_plan{
        .host_count = static_cast<std::size_t>(host_count),
        .exact_bytes = typed_bytes,
        .preallocation_bytes = *preallocation_bytes,
    };
}

/** Returns writable record bytes after checking the vector size against the download plan. */
template <class p_minimizer_type>
[[nodiscard]] std::span<std::byte> exact_typed_destination_span(
    std::vector<p_minimizer_type>& destination,
    const typed_destination_plan& plan) {
    if (destination.size() != plan.host_count) {
        throw vulkan_runtime_error{"authorized typed query destination was not resized"};
    }
    auto bytes = std::as_writable_bytes(std::span{destination});
    if (bytes.size_bytes() != plan.exact_bytes) {
        throw vulkan_runtime_error{"authorized typed query destination is not exact"};
    }
    return bytes;
}

/**
 * @brief Allocates a host record buffer and fills it through a synchronous callback.
 *
 * Requires an empty vector without reserved capacity or a memory permit. Reserves
 * the planned bytes, accounts for actual vector capacity, and touches that  capacity
 * before measuring process memory. The callback receives only the requested records.
 * @tparam p_footprint_probe Probe used to measure process memory.
 * @param transfer Fills the byte span without changing vector size or capacity.
 * @throws vulkan_runtime_error If reservation, allocation checks, or callback checks fail.
 */
template <
    class p_minimizer_type,
    bioseq::parallelism::memory_footprint_probe p_footprint_probe =
        bioseq::parallelism::live_process_footprint_probe,
    class p_transfer>
void materialize_typed_destination(
    const typed_destination_plan& plan,
    bioseq::parallelism::memory_permit& pending_permit,
    std::vector<p_minimizer_type>& pending,
    p_transfer&& transfer) {
    if (pending_permit || !pending.empty() || pending.capacity() != 0u) {
        throw vulkan_runtime_error{"authorized typed query destination is not fresh"};
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    if (plan.host_count != 0u) {
        auto reserved = admission.try_reserve(plan.preallocation_bytes);
        if (!reserved.has_value()) {
            throw vulkan_runtime_error{"authorized typed query host admission was denied"};
        }
        pending_permit = std::move(*reserved);
        (void)admission.materialize(
            pending_permit,
            [&pending,
             &pending_permit,
             host_count = plan.host_count,
             preallocation_bytes = plan.preallocation_bytes] {
                pending.reserve(host_count);
                const auto actual_bytes = bioseq::common::checked_mul(
                    static_cast<std::uint64_t>(pending.capacity()),
                    sizeof(p_minimizer_type));
                if (!actual_bytes.has_value() || *actual_bytes > preallocation_bytes ||
                    pending.capacity() < host_count) {
                    throw vulkan_runtime_error{
                        "authorized typed query capacity exceeded its admitted bound"};
                }
                pending_permit.shrink_to(*actual_bytes);
                if (pending_permit.bytes() != *actual_bytes) {
                    throw vulkan_runtime_error{
                        "authorized typed query permit reconciliation failed"};
                }
                pending.resize(pending.capacity());
                std::memset(pending.data(), 0, static_cast<std::size_t>(*actual_bytes));
                pending.resize(host_count);
                return bioseq::parallelism::memory_materialization_snapshot{
                    .actual_allocation_bytes = *actual_bytes,
                    .fully_touched_bytes = *actual_bytes,
                    .is_host_visible = true,
                    .is_host_coherent = true,
                    .is_process_footprint_reflected = true,
                };
            },
            p_footprint_probe);
    }

    const std::size_t retained_capacity = pending.capacity();
    std::span<std::byte> destination = exact_typed_destination_span(pending, plan);
    std::forward<p_transfer>(transfer)(destination);
    if (pending.size() != plan.host_count || pending.capacity() != retained_capacity) {
        throw vulkan_runtime_error{
            "authorized typed query readback changed its destination extent"};
    }
}

} // namespace bioseq::gpu::vulkan::authorized_readback_detail

namespace bioseq::gpu::vulkan::batch_admission_detail {

/**
 * @brief Estimates mmory for query records and their tile counts.
 * @param query_count Number of input queries.
 * @return Estimated vector bytes including growth allowance, clamped on overflow.
 */
[[nodiscard]] constexpr std::uint64_t query_authority_headroom_bytes(
    std::uint64_t query_count) noexcept {
    return bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(
            query_count,
            sizeof(gpu_seed_query_record_wire) + sizeof(std::uint32_t)),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
}

} // namespace bioseq::gpu::vulkan::batch_admission_detail

export namespace bioseq::gpu::vulkan {

/**
 * @brief Sketches a contiguous packed sequence and returns host and device records.
 *
 * Checks alphabet and k-mer/window sizes before preparing the upload.
 * @param seq Source sequence.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param decline_out Optional  rejection details.
 * @param host_ownership_route How the caller will keep the host result alive.
 * @return Sketch result, or no value when the input or resources are unsuitable.
 * @throws vulkan_runtime_error If execution or result checks fail
 */
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_result_resident_impl(
    const p_sequence& seq,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_decline* decline_out = nullptr,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    using bare = std::remove_cvref_t<p_sequence>;
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = vulkan_decline_bp_size(seq),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
        }
    };
    if constexpr (!vulkan_sketchable_sequence<bare>) {
        decline(vulkan_sketch_decline_reason::sequence_type_unsupported);
        return std::nullopt;
    } else {
        using traits = typename bare::traits;
        if constexpr (!vulkan_minimizer_alphabet_shape_supported<traits>()) {
            decline(vulkan_sketch_decline_reason::alphabet_unsupported);
            return std::nullopt;
        } else {
            if (!vulkan_minimizer_kmer_size_supported<traits>(kmer_size)) {
                decline(vulkan_sketch_decline_reason::kmer_size_unsupported);
                return std::nullopt;
            }
            if (!vulkan_minimizer_window_size_supported(window_size)) {
                decline(vulkan_sketch_decline_reason::window_size_unsupported);
                return std::nullopt;
            }
            const sequence_upload_view upload = make_sequence_upload_view(seq);
            const vulkan_rank_lut rank_lut = make_vulkan_rank_lut<bare>();
            return sketch_uploaded_window_result<p_minimizer_type>(
                upload,
                rank_lut,
                window_size,
                kmer_size,
                0u,
                nullptr,
                {},
                decline_out,
                nullptr,
                0u,
                host_ownership_route);
        }
    }
}

/** Direct sequence batches are disabled. Prepare upload descriptors before batching. */
template <class p_minimizer_type, class p_sequence>
[[nodiscard]] std::optional<vulkan_batch_sketch_result<p_minimizer_type>>
collect_minimizers_vulkan_batch_result(
    std::span<const p_sequence> seqs,
    std::size_t window_size,
    std::size_t kmer_size) = delete;

/**
 * @brief Holds input sizes and the memory reserved for an accepted batch prefix.
 *
 * The caller owns the upload descriptors and sequence bytes until execution
 * finishes. Execution moves the memory reservation into the result owner.
 */
template <class p_minimizer_type>
struct vulkan_upload_batch_admission_plan {
    std::span<const sequence_upload_view> uploads{};
    std::size_t accepted_uploads = 0u;
    vulkan_rank_lut rank_lut{};
    std::size_t window_size = 0u;
    std::size_t kmer_size = 0u;
    vulkan_sketch_host_materialization materialization =
        vulkan_sketch_host_materialization::host_and_device;
    std::uint64_t packed_sequence_bytes = 0u;
    std::uint64_t total_tile_count = 0u;
    std::uint64_t max_minimizers = 0u;
    std::uint64_t max_bp = 0u;
    std::uint32_t letters_per_unit = 0u;
    std::uint32_t unit_bits = 0u;
    std::uint32_t bits_per_char = 0u;
    std::uint32_t char_mask = 0u;
    std::uint32_t alphabet_size = 0u;
    std::uint32_t padding_letters = 0u;
    std::uint32_t alphabet_kind = 0u;
    bioseq::parallelism::memory_permit permit{};

    vulkan_upload_batch_admission_plan() = default;
    vulkan_upload_batch_admission_plan(
        const vulkan_upload_batch_admission_plan&) = delete;
    vulkan_upload_batch_admission_plan& operator=(
        const vulkan_upload_batch_admission_plan&) = delete;
    vulkan_upload_batch_admission_plan(
        vulkan_upload_batch_admission_plan&&) noexcept = default;
    vulkan_upload_batch_admission_plan& operator=(
        vulkan_upload_batch_admission_plan&&) noexcept = default;

    /** @brief Returns the ordered descriptor count covered by this plan. */
    [[nodiscard]] std::size_t accepted_upload_count() const noexcept {
        return accepted_uploads;
    }

    /** Binds accepted descriptors if  their count matches. The caller must supply the checked inputs. */
    [[nodiscard]] bool bind_accepted_uploads(
        std::span<const sequence_upload_view> accepted) noexcept {
        if (accepted.size() != accepted_uploads) {
            return false;
        }
        uploads = accepted;
        return true;
    }
};

/**
 * @brief Starts an empty batch plan for a supported device and k-mer/window size.
 * @param rank_lut Shared alphabet decoding table.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param materialization Whether to return host records as well as device records.
 * @return Empty plan, or no value for unsupported yizes or unavailable hardware.
 * @throws vulkan_runtime_error If materialization is invalid.
 */
template <class p_minimizer_type = ::bioseq::minimizers::minimap_sketch_data>
[[nodiscard]] std::optional<vulkan_upload_batch_admission_plan<p_minimizer_type>>
make_minimap_sketch_data_vulkan_upload_batch_admission_plan(
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_host_materialization materialization)
{
    if (materialization != vulkan_sketch_host_materialization::host_and_device
        && materialization != vulkan_sketch_host_materialization::device_only) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch has invalid host materialization mode"};
    }
    if (kmer_size == 0u || kmer_size > 32u
        || !vulkan_minimizer_window_size_supported(window_size)) {
        return std::nullopt;
    }
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime)
        || runtime.features().max_workgroup_count_x == 0u) {
        return std::nullopt;
    }
    vulkan_upload_batch_admission_plan<p_minimizer_type> plan;
    plan.rank_lut = rank_lut;
    plan.window_size = window_size;
    plan.kmer_size = kmer_size;
    plan.materialization = materialization;
    return plan;
}

/**
 * @brief Checks the next, input and reserves memory for adding it to the batch.
 *
 * All inputs must share packing and alphabet settings. A rejection leaves the
 * plan and its  reservation unchanged. No GPU work is submitted here.
 * @param plan Batch plan whose descriptors have not yet been bound.
 * @param upload Next borrowed input descriptor.
 * @return True when the input has been added to the accepted count.
 */
template <class p_minimizer_type>
[[nodiscard]] bool try_append_minimap_sketch_data_vulkan_upload(
    vulkan_upload_batch_admission_plan<p_minimizer_type>& plan,
    const sequence_upload_view& upload)
{
    using minimizer_type = p_minimizer_type;
    using query_tile_range = gpu_sketch_query_layout_wire;
    using bioseq::common::saturating_mul;

    if (!plan.uploads.empty()
        || plan.accepted_uploads
            >= static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())
        || !vulkan_upload_contract_supported(
            upload, plan.rank_lut, plan.kmer_size)
        || upload.bp_size > std::numeric_limits<std::uint32_t>::max()
        || !::bioseq::minimizers::is_minimizer_query_length_representable<
            minimizer_type>(upload.bp_size)
        || upload.padded_byte_size
            == (std::numeric_limits<std::uint64_t>::max)()
        || upload.padded_byte_size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())
        || upload.byte_size > upload.padded_byte_size
        || (upload.byte_size != 0u && upload.bytes == nullptr)) {
        return false;
    }

    const bool first_upload = plan.accepted_uploads == 0u;
    if (!first_upload
        && (upload.letters_per_unit != plan.letters_per_unit
            || upload.unit_bits != plan.unit_bits
            || upload.bits_per_char != plan.bits_per_char
            || upload.char_mask != plan.char_mask
            || upload.alphabet_size != plan.alphabet_size
            || static_cast<std::uint32_t>(upload.alphabet_kind)
                != plan.alphabet_kind)) {
        return false;
    }

    const std::uint64_t candidate_max_bp =
        std::max(plan.max_bp, upload.bp_size);
    const std::uint64_t candidate_max_minimizers =
        bioseq::common::saturating_add(
            plan.max_minimizers,
            estimate_vulkan_minimizer_capacity(
                upload.bp_size, plan.window_size));
    if (candidate_max_minimizers
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return false;
    }

    const std::uint64_t aligned_begin = bioseq::common::checked_align_up(
        plan.packed_sequence_bytes,
        sizeof(std::uint32_t))
        .value_or((std::numeric_limits<std::uint64_t>::max)());
    if (aligned_begin == (std::numeric_limits<std::uint64_t>::max)()
        || aligned_begin / sizeof(std::uint32_t)
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
        return false;
    }
    const std::uint64_t candidate_packed_sequence_bytes =
        bioseq::common::saturating_add(
            aligned_begin, upload.padded_byte_size);
    if (candidate_packed_sequence_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || candidate_packed_sequence_bytes
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return false;
    }

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    std::uint64_t candidate_total_tile_count = plan.total_tile_count;
    if (upload.bp_size >= plan.kmer_size) {
        const gpu_sketch_tile_plan tile_plan = make_sketch_tile_plan(
            upload.bp_size,
            static_cast<std::uint32_t>(plan.kmer_size),
            static_cast<std::uint32_t>(plan.window_size),
            upload.letters_per_unit,
            runtime.features().max_workgroup_count_x);
        if (!tile_plan.is_dispatchable || tile_plan.tile_count == 0u) {
            return false;
        }
        candidate_total_tile_count = bioseq::common::saturating_add(
            plan.total_tile_count, tile_plan.tile_count);
        if (candidate_total_tile_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
    }

    const std::size_t candidate_count = plan.accepted_uploads + 1u;
    if (candidate_total_tile_count != 0u
        && candidate_max_minimizers != 0u) {
        const gpu_sketch_batch_allocation_requirements requirements =
            runtime.sketch_batch_allocation_requirements(
                candidate_packed_sequence_bytes,
                candidate_total_tile_count,
                static_cast<std::uint32_t>(
                    minimizer_wire_traits<minimizer_type>::k_word_kind),
                minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
                candidate_max_minimizers);
        if (!requirements.valid) {
            return false;
        }
        const std::uint64_t pessimistic_host_minimizer_bytes =
            plan.materialization
                    == vulkan_sketch_host_materialization::host_and_device
                ? saturating_mul(
                    candidate_max_minimizers, sizeof(minimizer_type))
                : 0u;
        const std::uint64_t peak_host_minimizer_bytes = saturating_mul(
            pessimistic_host_minimizer_bytes,
            plan.materialization
                    == vulkan_sketch_host_materialization::host_and_device
                ? 2u
                : 0u);
        const std::uint64_t exact_scaffold_bytes =
            bioseq::common::saturating_add(
                bioseq::common::saturating_add(
                    candidate_packed_sequence_bytes,
                    saturating_mul(
                        candidate_total_tile_count,
                        sizeof(gpu_sketch_tile_wire))),
                bioseq::common::saturating_add(
                    bioseq::common::saturating_add(
                        saturating_mul(
                            candidate_count, sizeof(query_tile_range)),
                        bioseq::common::saturating_add(
                            saturating_mul(
                                candidate_count, sizeof(gpu_seed_query_record_wire)),
                            batch_admission_detail::query_authority_headroom_bytes(
                                candidate_count))),
                    saturating_mul(
                        candidate_total_tile_count,
                        2u * sizeof(std::uint32_t))));
        const std::uint64_t initial_peak_bytes =
            bioseq::common::saturating_add(
                bioseq::common::saturating_add(
                    requirements.arena_allocation_bytes,
                    requirements.pessimistic_output_allocation_bytes),
                bioseq::common::saturating_add(
                    peak_host_minimizer_bytes,
                    exact_scaffold_bytes));
        if (initial_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        bool admitted = false;
        if (plan.permit) {
            admitted = plan.permit.grow_to(initial_peak_bytes);
        } else {
            auto initial_permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(initial_peak_bytes);
            if (initial_permit) {
                plan.permit = std::move(*initial_permit);
                admitted = true;
            }
        }
        if (!admitted) {
            return false;
        }
    }

    plan.accepted_uploads = candidate_count;
    plan.packed_sequence_bytes = candidate_packed_sequence_bytes;
    plan.total_tile_count = candidate_total_tile_count;
    plan.max_minimizers = candidate_max_minimizers;
    plan.max_bp = candidate_max_bp;
    if (first_upload) {
        plan.letters_per_unit = upload.letters_per_unit;
        plan.unit_bits = upload.unit_bits;
        plan.bits_per_char = upload.bits_per_char;
        plan.char_mask = upload.char_mask;
        plan.alphabet_size = upload.alphabet_size;
        plan.padding_letters = upload.padding_letters;
        plan.alphabet_kind =
            static_cast<std::uint32_t>(upload.alphabet_kind);
    }
    return true;
}

/**
 * @brief Reserves memory for consecutive inputs until one cannot be accepted.
 *
 * Stops at the first invalid input or failed reservation. A nonempty batch must
 * contain at least one dispatchable tile Empty input returns an empty plan.
 * @param candidate_uploads Inputs in order.
 * @param rank_lut Shared alphabet decoding table.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param materialization Whether host records are needed.
 * @return Accepted  prefix, or no value when no dispatchable prefix fits.
 * @throws vulkan_runtime_error If materialization is invalid.
 */
template <class p_minimizer_type = ::bioseq::minimizers::minimap_sketch_data>
[[nodiscard]] std::optional<vulkan_upload_batch_admission_plan<p_minimizer_type>>
admit_minimap_sketch_data_vulkan_upload_batch_prefix(
    std::span<const sequence_upload_view> candidate_uploads,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_host_materialization materialization)
{
    using minimizer_type = p_minimizer_type;

    if (materialization != vulkan_sketch_host_materialization::host_and_device
        && materialization != vulkan_sketch_host_materialization::device_only) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch has invalid host materialization mode"};
    }
    if (candidate_uploads.empty()) {
        vulkan_upload_batch_admission_plan<minimizer_type> empty;
        empty.rank_lut = rank_lut;
        empty.window_size = window_size;
        empty.kmer_size = kmer_size;
        empty.materialization = materialization;
        return empty;
    }
    if (candidate_uploads.size()
        > static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return std::nullopt;
    }

    auto plan =
        make_minimap_sketch_data_vulkan_upload_batch_admission_plan<
            minimizer_type>(rank_lut, window_size, kmer_size, materialization);
    if (!plan) {
        return std::nullopt;
    }
    for (const sequence_upload_view& upload : candidate_uploads) {
        if (!try_append_minimap_sketch_data_vulkan_upload(*plan, upload)) {
            break;
        }
    }
    if (plan->accepted_upload_count() == 0u
        || plan->total_tile_count == 0u
        || plan->max_minimizers == 0u
        || !plan->bind_accepted_uploads(
            candidate_uploads.first(plan->accepted_upload_count()))) {
        return std::nullopt;
    }
    return plan;
}

/**
 * @brief Executes a prepared batch using its memory reservation.
 *
 * Consumes the plan reservatio and keeps it with the returned buffers. Actual
 * allocation sizes are checked before dispatch and again before results are returned.
 * @param plan Plan with the accepted input descriptors bound.
 * @return Batch result, or no value for output overflow or failed memory checks.
 * @throws vulkan_runtime_error If execution or result-consistency checks fail.
 */
template <class p_minimizer_type = ::bioseq::minimizers::minimap_sketch_data>
[[nodiscard]] std::optional<vulkan_batch_sketch_result<p_minimizer_type>>
execute_admitted_minimap_sketch_data_vulkan_upload_batch(
    vulkan_upload_batch_admission_plan<p_minimizer_type>&& plan)
{
    using minimizer_type = p_minimizer_type;
    using bioseq::common::saturating_mul;
    using query_tile_range = gpu_sketch_query_layout_wire;

    const std::span<const sequence_upload_view> uploads = plan.uploads;
    const vulkan_rank_lut rank_lut = plan.rank_lut;
    const std::size_t window_size = plan.window_size;
    const std::size_t kmer_size = plan.kmer_size;
    const vulkan_sketch_host_materialization materialization = plan.materialization;
    const std::uint64_t packed_sequence_bytes = plan.packed_sequence_bytes;
    const std::uint64_t total_tile_count = plan.total_tile_count;
    const std::uint64_t max_minimizers = plan.max_minimizers;
    const std::uint64_t max_bp = plan.max_bp;
    const std::uint32_t letters_per_unit = plan.letters_per_unit;
    const std::uint32_t unit_bits = plan.unit_bits;
    const std::uint32_t bits_per_char = plan.bits_per_char;
    const std::uint32_t char_mask = plan.char_mask;
    const std::uint32_t alphabet_size = plan.alphabet_size;
    const std::uint32_t padding_letters = plan.padding_letters;
    const std::uint32_t alphabet_kind = plan.alphabet_kind;

    if (uploads.size() != plan.accepted_upload_count()) {
        throw vulkan_runtime_error{
            "Vulkan batch execution descriptor count differs from admission"};
    }
    if (uploads.empty()) {
        return vulkan_batch_sketch_result<minimizer_type>{materialization};
    }
    if (!plan.permit || total_tile_count == 0u || max_minimizers == 0u) {
        throw vulkan_runtime_error{"Vulkan batch execution received an unadmitted plan"};
    }
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();

    /** @brief Returns vector reserved capacity in bytes, or UINT64_MAX on overflow. */
    const auto vector_capacity_bytes = []<class p_value_type>(
        const std::vector<p_value_type>& values) noexcept -> std::uint64_t {
        if (values.capacity()
            > (std::numeric_limits<std::uint64_t>::max)()) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            sizeof(p_value_type));
    };
    const auto release_vector_storage = []<class p_value_type>(
        std::vector<p_value_type>& values) noexcept {
        std::vector<p_value_type>{}.swap(values);
    };
    const std::uint64_t pessimistic_host_minimizer_bytes =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? saturating_mul(max_minimizers, sizeof(minimizer_type))
        : 0u;
    const std::uint64_t peak_host_minimizer_bytes = saturating_mul(
        pessimistic_host_minimizer_bytes,
        materialization == vulkan_sketch_host_materialization::host_and_device
            ? 2u
            : 0u);

    // Keep the reservation  owner alive until every result and temporary buffer
    // has been freed, including when an error leaves this function early.
    auto memory_retention =
        std::make_shared<vulkan_batch_sketch_memory_retention>();
    memory_retention->permit = std::move(plan.permit);
    vulkan_batch_sketch_result<minimizer_type> result{
        memory_retention, materialization,
    };
    std::vector<std::byte> packed_sequence;
    std::vector<gpu_sketch_tile_wire> tiles;
    std::vector<query_tile_range> tile_ranges;

    result.query_records.resize(uploads.size());
    packed_sequence.reserve(
        static_cast<std::size_t>(packed_sequence_bytes));
    tiles.reserve(static_cast<std::size_t>(total_tile_count));
    tile_ranges.resize(uploads.size());

    // Fill buffers using the sizes computed during planning. Check their actual
    // capacities against the memory reservation before submitting work.
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const sequence_upload_view& upload = uploads[query_index];
        const std::size_t aligned_begin = static_cast<std::size_t>(
            bioseq::common::checked_align_up(
                packed_sequence.size(),
                sizeof(std::uint32_t))
                .value_or((std::numeric_limits<std::size_t>::max)()));
        packed_sequence.resize(aligned_begin);
        const std::uint32_t seq_word_offset =
            static_cast<std::uint32_t>(
                aligned_begin / sizeof(std::uint32_t));
        const std::size_t append_begin = packed_sequence.size();
        const std::size_t padded_bytes =
            static_cast<std::size_t>(upload.padded_byte_size);
        packed_sequence.resize(append_begin + padded_bytes);
        if (padded_bytes != 0u) {
            std::memset(
                packed_sequence.data() + append_begin,
                0,
                padded_bytes);
        }
        if (upload.byte_size != 0u) {
            std::memcpy(
                packed_sequence.data() + append_begin,
                upload.bytes,
                static_cast<std::size_t>(upload.byte_size));
        }

        const std::uint32_t tile_begin =
            static_cast<std::uint32_t>(tiles.size());
        if (upload.bp_size >= kmer_size) {
            const gpu_sketch_tile_plan tile_plan = make_sketch_tile_plan(
                upload.bp_size,
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size),
                upload.letters_per_unit,
                runtime.features().max_workgroup_count_x);
            const std::uint32_t bp =
                static_cast<std::uint32_t>(upload.bp_size);
            const std::uint32_t overlap = sketch_replay_overlap_bp(
                static_cast<std::uint32_t>(kmer_size),
                static_cast<std::uint32_t>(window_size));
            for (std::uint32_t tile = 0u; tile < tile_plan.tile_count; ++tile) {
                const std::uint32_t lo = tile * tile_plan.tile_bp;
                const std::uint32_t hi =
                    std::min(bp, (tile + 1u) * tile_plan.tile_bp);
                tiles.push_back(gpu_sketch_tile_wire{
                    .seq_word_offset = seq_word_offset,
                    .bp_count = bp,
                    .replay_lo = lo > overlap ? lo - overlap : 0u,
                    .tile_lo = lo,
                    .tile_hi = hi,
                    .out_base = 0u,
                });
            }
        }
        tile_ranges[query_index] = query_tile_range{
            .begin = tile_begin,
            .count = static_cast<std::uint32_t>(
                tiles.size() - tile_begin),
            .query_len = static_cast<std::uint32_t>(upload.bp_size),
        };
    }
    if (packed_sequence.size() != packed_sequence_bytes
        || tiles.size() != total_tile_count) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch preflight/fill shape mismatch"};
    }

    const gpu_sketch_batch_allocation_requirements pre_dispatch_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!pre_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t actual_scaffold_capacity_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(tile_ranges),
            bioseq::common::saturating_add(
                vector_capacity_bytes(result.query_records),
                vector_capacity_bytes(result.host_minimizers))));
    const std::uint64_t anticipated_raw_count_bytes = saturating_mul(
        total_tile_count,
        2u * sizeof(std::uint32_t));
    const std::uint64_t authority_query_record_headroom =
        batch_admission_detail::query_authority_headroom_bytes(uploads.size());
    const std::uint64_t pre_dispatch_peak_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            pre_dispatch_requirements.arena_allocation_bytes,
            pre_dispatch_requirements.pessimistic_output_allocation_bytes),
        bioseq::common::saturating_add(
            peak_host_minimizer_bytes,
            bioseq::common::saturating_add(
                actual_scaffold_capacity_bytes,
                bioseq::common::saturating_add(
                    anticipated_raw_count_bytes,
                    authority_query_record_headroom))));
    bioseq::parallelism::memory_admission_failure growth_failure{};
    if (pre_dispatch_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            pre_dispatch_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    const vulkan_sketch_params sketch_params{
        .bp_size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                max_bp,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::uint32_t>::max)()))),
        .window_size = static_cast<std::uint32_t>(window_size),
        .kmer_size = static_cast<std::uint32_t>(kmer_size),
        .hash_bits = minimizer_wire_traits<minimizer_type>::k_hash_bits,
        .hash_shift = minimizer_wire_traits<minimizer_type>::k_hash_shift,
        .pos_bits = minimizer_wire_traits<minimizer_type>::k_position_bits,
        .minimizer_word_kind = static_cast<std::uint32_t>(
            minimizer_wire_traits<minimizer_type>::k_word_kind),
        .minimizer_words32 =
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
        .alphabet_kind = alphabet_kind,
        .unit_bits = unit_bits,
        .bits_per_char = bits_per_char,
        .letters_per_unit = letters_per_unit,
        .alphabet_size = alphabet_size,
        .seq_word_offset = 0u,
        .char_mask = char_mask,
        .padding_letters = padding_letters,
    };

    // Destroy the raw runtime buffers before temporary host buffers and the
    // shared reservation if a later check fails.
    runtime_sketch_batch_result raw =
        runtime.run_sketch_tile_batch_roundtrip(
            std::span<const std::byte>{
                packed_sequence.data(),
                packed_sequence.size()},
            rank_lut,
            sketch_params,
            std::span<const gpu_sketch_tile_wire>{tiles.data(), tiles.size()},
            std::span<const gpu_sketch_query_layout_wire>{
                tile_ranges.data(), tile_ranges.size()},
            max_minimizers,
            materialization);
    const gpu_sketch_batch_allocation_requirements post_dispatch_requirements =
        runtime.sketch_batch_allocation_requirements(
            packed_sequence_bytes,
            total_tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<minimizer_type>::k_word_kind),
            minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!post_dispatch_requirements.valid) return std::nullopt;
    const std::uint64_t post_dispatch_live_vector_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(packed_sequence),
            vector_capacity_bytes(tiles)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(tile_ranges),
            bioseq::common::saturating_add(
                bioseq::common::saturating_add(
                    vector_capacity_bytes(result.query_records),
                    vector_capacity_bytes(result.host_minimizers)),
                bioseq::common::saturating_add(
                    vector_capacity_bytes(raw.host_minimizer_bytes),
                    bioseq::common::saturating_add(
                        vector_capacity_bytes(raw.tile_counts),
                        bioseq::common::saturating_add(
                            vector_capacity_bytes(raw.tile_offsets),
                            raw.query_authority.owned_capacity_bytes()))))));
    const std::uint64_t post_dispatch_live_bytes = bioseq::common::saturating_add(
        raw.minimizer_words.allocation_bytes,
        bioseq::common::saturating_add(
            post_dispatch_requirements.arena_allocation_bytes,
            post_dispatch_live_vector_bytes));
    if (post_dispatch_live_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            post_dispatch_live_bytes,
            &growth_failure)) {
        return std::nullopt;
    }
    if (raw.overflow) {
        return std::nullopt;
    }
    if (raw.dispatch_stats.materialization() != materialization
        || !raw.dispatch_stats.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan batch sketch returned without matching is_internally_consistent "
            "count/scan/emit stats"};
    }
    if (materialization == vulkan_sketch_host_materialization::device_only
        && !raw.query_authority.does_cover_device_output()) {
        throw vulkan_runtime_error{
            "device-only Vulkan batch sketch returned without runtime-issued "
            "device-output coverage authority"};
    }

    if (materialization == vulkan_sketch_host_materialization::host_and_device) {
        result.host_minimizers = decode_host_minimizer_bytes<minimizer_type>(
            std::span<const std::byte>{
                raw.host_minimizer_bytes.data(),
                raw.host_minimizer_bytes.size()},
            raw.count);
    } else if (!raw.host_minimizer_bytes.empty()) {
        throw vulkan_runtime_error{
            "device-only Vulkan batch sketch unexpectedly materialized host hashes"};
    }
    std::uint64_t query_minimizer_cursor = 0u;
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const query_tile_range range = tile_ranges[query_index];
        std::uint64_t minimizer_count = 0u;
        for (std::uint32_t tile = 0u; tile < range.count; ++tile) {
            minimizer_count += raw.tile_counts[range.begin + tile];
        }
        const std::uint64_t minimizer_offset = range.count == 0u
            ? query_minimizer_cursor
            : static_cast<std::uint64_t>(raw.tile_offsets[range.begin]);
        if (minimizer_offset != query_minimizer_cursor) {
            throw vulkan_runtime_error{
                "Vulkan batch query minimizer slices are not contiguous"};
        }
        if (minimizer_count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw vulkan_runtime_error{
                "Vulkan batch query minimizer count exceeds ABI"};
        }
        if (uploads[query_index].bp_size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw vulkan_runtime_error{"Vulkan batch query length exceeds ABI"};
        }
        result.query_records[query_index] = gpu_seed_query_record_wire{
            .minimizer_offset = static_cast<std::uint32_t>(minimizer_offset),
            .minimizer_count = static_cast<std::uint32_t>(minimizer_count),
            .query_len = static_cast<std::uint32_t>(uploads[query_index].bp_size),
            .flags = 0u,
        };
        query_minimizer_cursor = bioseq::common::saturating_add(
            query_minimizer_cursor,
            minimizer_count);
    }
    if (query_minimizer_cursor != raw.count) {
        throw vulkan_runtime_error{
            "Vulkan batch query minimizer slices do not cover device output"};
    }

    const std::uint64_t retained_host_capacity_bytes =
        vector_capacity_bytes(result.host_minimizers);
    const std::uint64_t retained_query_record_capacity_bytes =
        vector_capacity_bytes(result.query_records);
    const std::uint64_t retained_bytes = bioseq::common::saturating_add(
        raw.minimizer_words.allocation_bytes,
        bioseq::common::saturating_add(
            retained_host_capacity_bytes,
            bioseq::common::saturating_add(
                retained_query_record_capacity_bytes,
                raw.query_authority.owned_capacity_bytes())));
    const std::uint64_t transient_vector_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(
            vector_capacity_bytes(raw.host_minimizer_bytes),
            vector_capacity_bytes(raw.tile_counts)),
        bioseq::common::saturating_add(
            vector_capacity_bytes(raw.tile_offsets),
            bioseq::common::saturating_add(
                vector_capacity_bytes(packed_sequence),
                bioseq::common::saturating_add(
                    vector_capacity_bytes(tiles),
                    vector_capacity_bytes(tile_ranges)))));
    const std::uint64_t final_live_peak_bytes = bioseq::common::saturating_add(
        post_dispatch_requirements.arena_allocation_bytes,
        bioseq::common::saturating_add(retained_bytes, transient_vector_bytes));
    if (final_live_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !memory_retention->permit.grow_to(
            final_live_peak_bytes,
            &growth_failure)) {
        return std::nullopt;
    }

    release_vector_storage(raw.host_minimizer_bytes);
    release_vector_storage(raw.tile_counts);
    release_vector_storage(raw.tile_offsets);
    release_vector_storage(packed_sequence);
    release_vector_storage(tiles);
    release_vector_storage(tile_ranges);
    memory_retention->permit.shrink_to(retained_bytes);

    auto storage =
        std::make_shared<vulkan_device_minimizer_storage<minimizer_type>>();
    storage->words = std::move(raw.minimizer_words);
    storage->batch_memory_retention = memory_retention;
    storage->count = raw.count;
    storage->capacity =
        materialization == vulkan_sketch_host_materialization::host_and_device
        ? max_minimizers
        : raw.count;
    result.device = std::move(storage);
    result.dispatch_stats = raw.dispatch_stats;
    result.query_authority = std::move(raw.query_authority);
    result.device_minimizer_count = raw.count;
    return result;
}

/**
 * @brief Sketches a batch only when all prepared inputs can be accepted together.
 * @param uploads Query inputs in, order.
 * @param rank_lut Shared alphabet decoding table.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per  k-mer.
 * @param materialization Whether host records are needed.
 * @return Batch result, or no valup when planning or execution rejects the batch
 * @throws vulkan_runtime_error If execution or result-consistency checks fail.
 */
template <class p_minimizer_type = ::bioseq::minimizers::minimap_sketch_data>
[[nodiscard]] std::optional<vulkan_batch_sketch_result<p_minimizer_type>>
collect_minimap_sketch_data_vulkan_upload_batch_result(
    std::span<const sequence_upload_view> uploads,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    vulkan_sketch_host_materialization materialization)
{
    auto plan = admit_minimap_sketch_data_vulkan_upload_batch_prefix<p_minimizer_type>(
        uploads,
        rank_lut,
        window_size,
        kmer_size,
        materialization);
    if (!plan || plan->accepted_upload_count() != uploads.size()) {
        return std::nullopt;
    }
    return execute_admitted_minimap_sketch_data_vulkan_upload_batch<p_minimizer_type>(
        std::move(*plan));
}

/**
 * @brief Downloads an existing device-only query sketch into its host result buffer.
 *
 * Allows one successful download per result. The runtime checks its readback
 * permission and leaves it valid for the later seeding call. No sketch is rerun.
 * Keep the full batch alive, and expose its host subranges only after this succeeds.
 * @throws vulkan_runtime_error If ownership, record ranges, or download checks fail.
 */
template <class p_minimizer_type>
void materialize_authorized_query_minimizers(
    vulkan_batch_sketch_result<p_minimizer_type>& result,
    runtime& issuing_runtime) {
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    using wire = minimizer_wire_traits<p_minimizer_type>;
    constexpr std::uint64_t record_bytes =
        static_cast<std::uint64_t>(wire::k_words32_per_minimizer) * sizeof(std::uint32_t);
    if (sizeof(p_minimizer_type) != record_bytes ||
        result.materialization != vulkan_sketch_host_materialization::device_only ||
        result.overflow || result.host_readback_completed ||
        !result.host_minimizers.empty() || result.host_minimizers.capacity() != 0u ||
        result.host_readback_permit || result.memory_retention == nullptr ||
        result.device == nullptr || !result.query_authority.is_valid() ||
        result.query_records.empty() ||
        result.query_records.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        result.device->count != result.device_minimizer_count ||
        result.device->word_kind != wire::k_word_kind ||
        result.device->words32_per_minimizer != wire::k_words32_per_minimizer) {
        throw vulkan_runtime_error{"authorized typed query readback has invalid ownership or shape"};
    }
    const std::uint64_t host_count_u64 = result.device_minimizer_count;
    const auto destination_plan = authorized_readback_detail::make_typed_destination_plan<
        p_minimizer_type>(
        host_count_u64,
        record_bytes,
        result.query_authority.logical_minimizer_bytes(),
        result.device->words.bytes,
        result.device->words.allocation_bytes,
        result.host_minimizers.max_size());
    std::uint64_t prefix = 0u;
    for (const auto& record : result.query_records) {
        if (record.flags != 0u || record.minimizer_offset != prefix ||
            prefix > host_count_u64 || record.minimizer_count > host_count_u64 - prefix) {
            throw vulkan_runtime_error{"authorized typed query records are not a complete partition"};
        }
        prefix += record.minimizer_count;
    }
    if (prefix != host_count_u64) {
        throw vulkan_runtime_error{"authorized typed query records do not cover the device output"};
    }

    // Reserve  space once and check the allocated capacity. If process-memory
    // accounting still requires a reservation, keep it with the downloaded buffer.
    bioseq::parallelism::memory_permit pending_permit{};
    std::vector<p_minimizer_type> pending;
    authorized_readback_detail::materialize_typed_destination(
        destination_plan,
        pending_permit,
        pending,
        [&issuing_runtime,
         &result,
         host_count_u64](std::span<std::byte> destination) {
            // The process-memory measurement has finished. The runtime can now take
            // its own allocation lock for the download.
            issuing_runtime.readback_authorized_query_minimizers(
                result.query_authority,
                result.device->words,
                host_count_u64,
                static_cast<std::uint32_t>(wire::k_word_kind),
                wire::k_words32_per_minimizer,
                wire::k_hash_shift,
                wire::k_position_bits,
                result.query_records,
                destination);
        });
    if (!result.query_authority.is_valid()) {
        throw vulkan_runtime_error{"authorized typed query readback consumed its authority"};
    }
    // Move the buffer and its reservation before marking the download complete.
    result.host_minimizers.swap(pending);
    result.host_readback_permit = std::move(pending_permit);
    result.host_readback_completed = true;
}

#if defined(BIOSEQ_TEST_INDEXES)
/** Results of the host download-buffer checks used by test builds. */
struct authorized_typed_readback_test_audit {
    bool checked_multiplication_overflow_rejected = false;
    bool maximum_u32_count_accepted = false;
    bool wider_host_count_preserved = false;
    bool admission_denied_before_destination_or_transfer = false;
    bool pre_resize_span_rejected = false;
    bool actual_capacity_reconciled = false;
    bool exact_resized_destination_seen = false;
    bool backing_padding_excluded = false;
    bool successful_destination_extent_unchanged = false;
    bool mutated_destination_extent_rejected = false;
    bool negative_branch_storage_before_permit_cleanup = false;
};

/**
 * @brief Tests download-buffer planning and allocation  without a Vulkan device.
 *
 * Checks overflow, memory rejection, exact destination sizes, and cloanup.
 * @return Flags reporting which checks passed.
 */
[[nodiscard]] inline authorized_typed_readback_test_audit
self_test_authorized_typed_readback_contract() {
    using minimizer_type = ::bioseq::minimizers::minimap_sketch_data;
    using wire = minimizer_wire_traits<minimizer_type>;
    constexpr std::uint64_t record_bytes =
        static_cast<std::uint64_t>(wire::k_words32_per_minimizer) *
        sizeof(std::uint32_t);
    static_assert(sizeof(minimizer_type) == record_bytes);

    authorized_typed_readback_test_audit audit{};
    try {
        (void)authorized_readback_detail::checked_record_bytes(
            (std::numeric_limits<std::uint64_t>::max)(), 2u);
    } catch (const vulkan_runtime_error&) {
        audit.checked_multiplication_overflow_rejected = true;
    }

    std::vector<minimizer_type> capacity_probe;
    constexpr std::uint64_t maximum_count =
        (std::numeric_limits<std::uint32_t>::max)();
    const std::uint64_t maximum_exact_bytes =
        authorized_readback_detail::checked_record_bytes(maximum_count, record_bytes);
    try {
        const auto boundary =
            authorized_readback_detail::make_typed_destination_plan<minimizer_type>(
                maximum_count,
                record_bytes,
                maximum_exact_bytes,
                maximum_exact_bytes,
                maximum_exact_bytes,
                capacity_probe.max_size());
        audit.maximum_u32_count_accepted =
            boundary.host_count == static_cast<std::size_t>(maximum_count) &&
            boundary.exact_bytes == maximum_exact_bytes;
    } catch (const vulkan_runtime_error&) {
    }

    constexpr std::uint64_t above_maximum_count = maximum_count + 1u;
    const std::uint64_t above_maximum_bytes =
        authorized_readback_detail::checked_record_bytes(
            above_maximum_count, record_bytes);
    try {
        // Host planning accepts 64-bit record counts when they fit host limits.
        // GPU callers apply their 32-bit interface limits before this step.
        const auto wider = authorized_readback_detail::make_typed_destination_plan<minimizer_type>(
            above_maximum_count,
            record_bytes,
            above_maximum_bytes,
            above_maximum_bytes,
            above_maximum_bytes,
            capacity_probe.max_size());
        audit.wider_host_count_preserved =
            wider.host_count == above_maximum_count &&
            wider.exact_bytes == above_maximum_bytes;
    } catch (const vulkan_runtime_error&) {
    }

    constexpr std::uint64_t small_count = 5u;
    constexpr std::uint64_t padding_bytes = 64u;
    const std::uint64_t small_exact_bytes =
        authorized_readback_detail::checked_record_bytes(small_count, record_bytes);
    const auto plan =
        authorized_readback_detail::make_typed_destination_plan<minimizer_type>(
            small_count,
            record_bytes,
            small_exact_bytes,
            small_exact_bytes + padding_bytes,
            small_exact_bytes + 2u * padding_bytes,
            capacity_probe.max_size());

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous_cap = 0u;
        ~restore_cap_guard() {
            admission.set_index_memory_cap_bytes(previous_cap);
        }
    } restore{admission, previous_cap};

    const std::uint64_t denial_cap = baseline == 0u ? 1u : baseline;
    admission.set_index_memory_cap_bytes(denial_cap);
    {
        bioseq::parallelism::memory_permit pending_permit;
        std::vector<minimizer_type> pending;
        bool destination_seen = false;
        try {
            authorized_readback_detail::materialize_typed_destination<
                minimizer_type,
                nullptr>(
                plan,
                pending_permit,
                pending,
                [&destination_seen](std::span<std::byte>) {
                    destination_seen = true;
                });
        } catch (const vulkan_runtime_error&) {
            audit.admission_denied_before_destination_or_transfer =
                !destination_seen && !pending_permit && pending.capacity() == 0u;
        }
    }
    bool cleanup_is_exact = admission.reserved_bytes() == baseline;

    constexpr std::uint64_t cleanup_slack = 4096u;
    if (baseline > (std::numeric_limits<std::uint64_t>::max)() -
                       plan.preallocation_bytes - cleanup_slack) {
        return audit;
    }
    admission.set_index_memory_cap_bytes(
        baseline + plan.preallocation_bytes + cleanup_slack);

    {
        auto pre_resize_permit = admission.try_reserve(plan.preallocation_bytes);
        if (pre_resize_permit.has_value()) {
            std::vector<minimizer_type> pre_resize;
            pre_resize.reserve(plan.host_count);
            try {
                (void)authorized_readback_detail::exact_typed_destination_span(
                    pre_resize, plan);
            } catch (const vulkan_runtime_error&) {
                audit.pre_resize_span_rejected = true;
            }
            const bool storage_and_permit_were_live =
                pre_resize.capacity() >= plan.host_count &&
                static_cast<bool>(*pre_resize_permit);
            bioseq::parallelism::release_admitted_vector_capacity_before_permit(
                pre_resize, *pre_resize_permit);
            cleanup_is_exact = cleanup_is_exact && storage_and_permit_were_live &&
                               pre_resize.capacity() == 0u &&
                               !static_cast<bool>(*pre_resize_permit) &&
                               admission.reserved_bytes() == baseline;
        }
    }

    {
        bioseq::parallelism::memory_permit pending_permit;
        std::vector<minimizer_type> pending;
        std::size_t observed_capacity = 0u;
        authorized_readback_detail::materialize_typed_destination<
            minimizer_type,
            nullptr>(
            plan,
            pending_permit,
            pending,
            [&audit,
             &pending,
             &pending_permit,
             &plan,
             &observed_capacity,
             small_exact_bytes](std::span<std::byte> destination) {
                observed_capacity = pending.capacity();
                const std::uint64_t capacity_bytes =
                    authorized_readback_detail::checked_record_bytes(
                        static_cast<std::uint64_t>(pending.capacity()),
                        sizeof(minimizer_type));
                audit.actual_capacity_reconciled =
                    pending_permit.bytes() == capacity_bytes;
                audit.exact_resized_destination_seen =
                    pending.size() == plan.host_count &&
                    destination.size_bytes() == small_exact_bytes;
                audit.backing_padding_excluded =
                    destination.size_bytes() == small_exact_bytes &&
                    plan.exact_bytes + padding_bytes > destination.size_bytes();
            });
        audit.successful_destination_extent_unchanged =
            pending.size() == plan.host_count && pending.capacity() == observed_capacity;
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pending, pending_permit);
        cleanup_is_exact = cleanup_is_exact && pending.capacity() == 0u &&
                           !pending_permit && admission.reserved_bytes() == baseline;
    }

    {
        bioseq::parallelism::memory_permit pending_permit;
        std::vector<minimizer_type> pending;
        bool destination_seen = false;
        try {
            authorized_readback_detail::materialize_typed_destination<
                minimizer_type,
                nullptr>(
                plan,
                pending_permit,
                pending,
                [&destination_seen,
                 &pending](std::span<std::byte> destination) {
                    destination_seen = destination.size_bytes() != 0u;
                    pending.pop_back();
                });
        } catch (const vulkan_runtime_error&) {
            audit.mutated_destination_extent_rejected = destination_seen;
        }
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pending, pending_permit);
        cleanup_is_exact = cleanup_is_exact && pending.capacity() == 0u &&
                           !pending_permit && admission.reserved_bytes() == baseline;
    }
    audit.negative_branch_storage_before_permit_cleanup = cleanup_is_exact;
    return audit;
}
#endif

} // namespace bioseq::gpu::vulkan
