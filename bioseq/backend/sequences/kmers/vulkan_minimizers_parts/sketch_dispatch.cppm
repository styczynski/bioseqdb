/**
 * @file sketch_dispatch.cppm
 * @brief Sketches one Vulkan input window and downloads its minimizers.
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

export module bioseq.sequences.kmers.vulkan_minimizers:sketch_dispatch;

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

export namespace bioseq::gpu::vulkan {

/**
 * @brief Sketches one input window and returns host and device minimizer buffers.
 *
 * Checks or reserves dispatch,  memory first. Allocates the host result after
 * temporary upload storage is released. The call is synchronous.
 * @param upload Packed input kept alive through the call.
 * @param rank_lut Alphabet decoding table.
 * @param window_size K-mers per window.
 * @param kmer_size Symbols per k-mer.
 * @param capacity_bound Output capacity, or zero to estimate it.
 * @param capacity_overflowed Optional output-capacity failure flag.
 * @param query_gate Optional bitmap filtering target hashes.
 * @param decline_out Optional rejection details, including failures after dispatch.
 * @param borrowed_dispatch_permit Optional caller reservation kept alive with the returned buffers.
 * @param borrowed_dispatch_budget Bytes available  from that reservation.
 * @param host_ownership_route How the caller will keep the host buffer alive.
 * @return Buffers and completion details, or no value when the request is rejected.
 * @throws vulkan_runtime_error On execution failure, invalid result storage, or
 * overflow of an explicie capacity when capacity_overflowed was not supplied.
 */
template <class p_minimizer_type>
[[nodiscard]] std::optional<vulkan_sketch_result<p_minimizer_type>>
sketch_uploaded_window_result(
    const sequence_upload_view& upload,
    const vulkan_rank_lut& rank_lut,
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t capacity_bound = 0u,
    bool* capacity_overflowed = nullptr,
    vulkan_sketch_query_gate query_gate = {},
    vulkan_sketch_decline* decline_out = nullptr,
    bioseq::parallelism::memory_permit* borrowed_dispatch_permit = nullptr,
    std::uint64_t borrowed_dispatch_budget = 0u,
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible)
{
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::common::saturating_mul;

    if (capacity_overflowed != nullptr) {
        *capacity_overflowed = false;
    }
    if (decline_out != nullptr) {
        *decline_out = vulkan_sketch_decline{
            .bp_size = upload.bp_size,
            .padded_sequence_bytes = upload.padded_byte_size,
            .query_gate_words = static_cast<std::uint64_t>(query_gate.words.size()),
            .window_size = window_size,
            .kmer_size = kmer_size,
        };
    }
    /**
     * @brief Records a rejection when decline_out was supplied.
     * @param reason Failure reason.
     * @param max_minimizers Planned output capacity, if relevant.
     */
    const auto decline = [decline_out](
            vulkan_sketch_decline_reason reason,
            std::uint64_t max_minimizers = 0u) noexcept {
        if (decline_out != nullptr) {
            decline_out->reason = reason;
            decline_out->max_minimizers = max_minimizers;
        }
    };
    if (upload.bp_size > std::numeric_limits<std::uint32_t>::max()) {
        decline(vulkan_sketch_decline_reason::sequence_size_exceeds_u32);
        return std::nullopt;
    }
    if (upload.padded_byte_size == std::numeric_limits<std::uint64_t>::max()) {
        decline(vulkan_sketch_decline_reason::padded_sequence_size_overflow);
        return std::nullopt;
    }
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (!hardware_device_available(runtime)) {
        decline(vulkan_sketch_decline_reason::hardware_device_unavailable);
        return std::nullopt;
    }
    const std::uint64_t max_minimizers =
        capacity_bound == 0u
            ? estimate_vulkan_minimizer_capacity(upload.bp_size, window_size)
            : capacity_bound;
    if (max_minimizers > std::numeric_limits<std::uint32_t>::max()) {
        decline(
            vulkan_sketch_decline_reason::minimizer_capacity_exceeds_u32,
            max_minimizers);
        return std::nullopt;
    }
    if (decline_out != nullptr) {
        decline_out->max_minimizers = max_minimizers;
    }
    if (query_gate.words.size()
        > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        decline(
            vulkan_sketch_decline_reason::query_gate_exceeds_u32,
            max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_bytes =
        saturating_mul(
            static_cast<std::uint64_t>(query_gate.words.size()),
            static_cast<std::uint64_t>(sizeof(std::uint32_t)));
    const std::uint64_t requested_device_bytes =
        vulkan_sketch_resident_bytes<p_minimizer_type>(max_minimizers);
    const gpu_sketch_memory_plan dispatch_plan = make_gpu_sketch_memory_plan(
        upload.bp_size,
        upload.padded_byte_size,
        sizeof(vulkan_rank_lut),
        max_minimizers,
        requested_device_bytes,
        gpu_sketch_purpose::minimizer_words,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        upload.letters_per_unit,
        runtime.features().max_workgroup_count_x);
    if (requested_device_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || !dispatch_plan.is_valid) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const gpu_sketch_batch_allocation_requirements allocation_requirements =
        runtime.sketch_batch_allocation_requirements(
            upload.padded_byte_size,
            dispatch_plan.tiles.tile_count,
            static_cast<std::uint32_t>(
                minimizer_wire_traits<p_minimizer_type>::k_word_kind),
            minimizer_wire_traits<p_minimizer_type>::k_words32_per_minimizer,
            max_minimizers);
    if (!allocation_requirements.valid) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t query_gate_arena_bytes = query_gate_bytes == 0u
        ? 0u
        : bioseq::common::saturating_add(
            query_gate_bytes,
            std::uint64_t{k_gpu_buffer_offset_alignment});
    const std::uint64_t arena_request_bytes = bioseq::common::saturating_add(
        dispatch_plan.arena_scaffold_bytes,
        query_gate_arena_bytes);
    if (query_gate_arena_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || arena_request_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    const std::uint64_t physical_arena_bytes =
        runtime.gpu_arena_allocation_requirement(arena_request_bytes);
    const std::uint64_t host_transient_headroom_bytes =
        vulkan_exact_host_allocation_admission_bytes(
            dispatch_plan.count_submit_host_transient_bytes);
    const std::uint64_t resident_allocation_bytes =
        allocation_requirements.pessimistic_output_allocation_bytes;
    const std::uint64_t transient_bytes = bioseq::common::saturating_add(
        physical_arena_bytes,
        bioseq::common::saturating_add(host_transient_headroom_bytes, query_gate_bytes));

    if (host_transient_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || resident_allocation_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || transient_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }

    const std::uint64_t dispatch_admission_bytes =
        vulkan_sketch_dispatch_admission_bytes(
            physical_arena_bytes,
            host_transient_headroom_bytes,
            query_gate_bytes,
            resident_allocation_bytes);
    if (dispatch_admission_bytes
        == (std::numeric_limits<std::uint64_t>::max)()) {
        decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
        return std::nullopt;
    }
    bioseq::parallelism::memory_admission_failure admission_failure{};
    std::optional<bioseq::parallelism::memory_permit> owned_dispatch_permit;
    bioseq::parallelism::memory_permit* dispatch_permit = borrowed_dispatch_permit;
    if (dispatch_permit == nullptr) {
        owned_dispatch_permit = memory_admission_controller::instance().try_reserve(
            dispatch_admission_bytes,
            &admission_failure);
        if (owned_dispatch_permit) {
            dispatch_permit = std::addressof(*owned_dispatch_permit);
        }
    }
    if (dispatch_permit == nullptr
        || dispatch_permit->bytes() < dispatch_admission_bytes
        || (borrowed_dispatch_permit != nullptr
            && borrowed_dispatch_budget < dispatch_admission_bytes)) {
        if (decline_out != nullptr) {
            decline_out->reason =
                vulkan_sketch_decline_reason::memory_admission_denied;
            decline_out->admission = admission_failure;
        }
        bioseq_log<logging_level::error>(
            "[vulkan sketch] strict dispatch admission denied: {}",
            format_vulkan_sketch_decline(
                decline_out != nullptr
                    ? *decline_out
                    : vulkan_sketch_decline{
                        .reason = vulkan_sketch_decline_reason::memory_admission_denied,
                        .bp_size = upload.bp_size,
                        .padded_sequence_bytes = upload.padded_byte_size,
                        .max_minimizers = max_minimizers,
                        .query_gate_words = static_cast<std::uint64_t>(
                            query_gate.words.size()),
                        .window_size = window_size,
                        .kmer_size = kmer_size,
                        .admission = admission_failure,
                    }));
        return std::nullopt;
    }

    try {
        const vulkan_sketch_params sketch_params{
            .bp_size = static_cast<std::uint32_t>(upload.bp_size),
            .window_size = static_cast<std::uint32_t>(window_size),
            .kmer_size = static_cast<std::uint32_t>(kmer_size),
            .hash_bits = minimizer_wire_traits<p_minimizer_type>::k_hash_bits,
            .hash_shift = minimizer_wire_traits<p_minimizer_type>::k_hash_shift,
            .pos_bits = minimizer_wire_traits<p_minimizer_type>::k_position_bits,
            .minimizer_word_kind = static_cast<std::uint32_t>(
                minimizer_wire_traits<p_minimizer_type>::k_word_kind),
            .minimizer_words32 =
                minimizer_wire_traits<
                    p_minimizer_type>::k_words32_per_minimizer,
            .alphabet_kind = static_cast<std::uint32_t>(upload.alphabet_kind),
            .unit_bits = upload.unit_bits,
            .bits_per_char = upload.bits_per_char,
            .letters_per_unit = upload.letters_per_unit,
            .alphabet_size = upload.alphabet_size,
            .seq_word_offset = 0u,
            .char_mask = upload.char_mask,
            .padding_letters = upload.padding_letters,
        };

        runtime_sketch_result raw;
        {
            // Free the temporary packed input before reserving the host result
            // The runtime also releases its tile list and trims the GPU arena  before
            // returning from the dispatch.
            const auto padded_sequence_size =
                static_cast<std::size_t>(upload.padded_byte_size);
            auto padded_sequence_owner =
                std::make_unique_for_overwrite<std::byte[]>(
                    padded_sequence_size);
            std::span<std::byte> padded_sequence{
                padded_sequence_owner.get(),
                padded_sequence_size};
            if (upload.byte_size != 0u && upload.bytes != nullptr) {
                std::memcpy(
                    padded_sequence.data(),
                    upload.bytes,
                    static_cast<std::size_t>(upload.byte_size));
            }
            if (upload.padded_byte_size > upload.byte_size) {
                std::memset(
                    padded_sequence.data() +
                        static_cast<std::size_t>(upload.byte_size),
                    0,
                    static_cast<std::size_t>(
                        upload.padded_byte_size - upload.byte_size));
            }
            raw = runtime.run_sketch_roundtrip(
                std::span<const std::byte>{
                    padded_sequence.data(),
                    padded_sequence.size()},
                rank_lut,
                sketch_params,
                max_minimizers,
                query_gate.words,
                query_gate.mask);
        }
        if (!raw.arena_trimmed_before_return) {
            throw vulkan_runtime_error{
                "Vulkan minimizer roundtrip returned before arena trim"};
        }
        if (raw.overflow) {
            if (capacity_overflowed != nullptr) {
                *capacity_overflowed = true;
            }
            if (capacity_bound != 0u && capacity_overflowed == nullptr) {
                throw vulkan_runtime_error(fmt::format(
                    "Vulkan minimizer sketch overflowed explicit capacity bound "
                    "(bp={}, cap={})",
                    upload.bp_size,
                    max_minimizers));
            }
            decline(
                vulkan_sketch_decline_reason::output_capacity_overflow,
                max_minimizers);
            return std::nullopt;
        }

        const std::uint64_t actual_device_allocation_bytes =
            raw.minimizer_words.allocation_bytes;
        if (actual_device_allocation_bytes > resident_allocation_bytes) {
            throw vulkan_runtime_error{
                "Vulkan minimizer device allocation exceeded admission"};
        }
        if (borrowed_dispatch_permit == nullptr) {
            dispatch_permit->shrink_to(actual_device_allocation_bytes);
        }

        if (raw.count
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)()
                / sizeof(p_minimizer_type))) {
            throw vulkan_runtime_error{
                "Vulkan minimizer count exceeds host address space"};
        }
        const std::uint64_t exact_host_size_bytes = saturating_mul(
            raw.count,
            static_cast<std::uint64_t>(sizeof(p_minimizer_type)));
        const std::uint64_t exact_host_headroom_bytes =
            host_ownership_route
                    == vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
                ? vulkan_fixed_host_result_admission_bytes(
                      exact_host_size_bytes)
                : saturating_mul(
                      exact_host_size_bytes,
                      bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t host_materialization_peak_bytes =
            vulkan_sketch_host_materialization_admission_bytes(
                actual_device_allocation_bytes,
                exact_host_headroom_bytes);
        if (exact_host_size_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || exact_host_headroom_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || host_materialization_peak_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            decline(vulkan_sketch_decline_reason::memory_plan_invalid, max_minimizers);
            return std::nullopt;
        }

        std::optional<bioseq::parallelism::memory_permit> owned_host_permit;
        if (borrowed_dispatch_permit == nullptr) {
            bioseq::parallelism::memory_admission_failure host_failure{};
            owned_host_permit =
                memory_admission_controller::instance().try_reserve(
                    exact_host_headroom_bytes,
                    &host_failure);
            if (!owned_host_permit) {
                if (decline_out != nullptr) {
                    decline_out->reason =
                        vulkan_sketch_decline_reason::memory_admission_denied;
                    decline_out->admission = host_failure;
                }
                bioseq_log<logging_level::error>(
                    "[vulkan sketch] strict exact-host admission denied: {}",
                    format_vulkan_sketch_decline(
                        decline_out != nullptr
                            ? *decline_out
                            : vulkan_sketch_decline{
                                .reason = vulkan_sketch_decline_reason::
                                    memory_admission_denied,
                                .bp_size = upload.bp_size,
                                .padded_sequence_bytes =
                                    upload.padded_byte_size,
                                .max_minimizers = max_minimizers,
                                .query_gate_words =
                                    static_cast<std::uint64_t>(
                                        query_gate.words.size()),
                                .window_size = window_size,
                                .kmer_size = kmer_size,
                                .admission = host_failure,
                            }));
                return std::nullopt;
            }
        } else if (host_materialization_peak_bytes
                   > borrowed_dispatch_budget) {
            decline(vulkan_sketch_decline_reason::memory_admission_denied,
                    max_minimizers);
            return std::nullopt;
        }

        if (host_ownership_route
                == vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
            && borrowed_dispatch_permit != nullptr) {
            throw vulkan_runtime_error{
                "Vulkan fixed zero-copy host adoption requires an owned "
                "materialization permit"};
        }

        // The host reservation is declared before this vector so destruction frees
        // the buffer before returning its reserved bytes.
        std::vector<p_minimizer_type> host_minimizers;
        std::uint64_t exact_host_capacity_bytes = 0u;
        bool host_permit_released_to_process_footprint = false;
        std::uint64_t host_footprint_reflected_delta_bytes = 0u;
        /**
         * @brief Allocates the host result and downloads the device records into it.
         * @throws vulkan_runtime_error If capacity exceeds the reserved bytes, or the
         * fixed-adoption buffer has spare capacity.
         */
        const auto allocate_and_download = [
                &host_minimizers,
                &exact_host_capacity_bytes,
                &raw,
                host_ownership_route,
                exact_host_headroom_bytes,
                &owned_host_permit,
                &runtime] {
            host_minimizers.resize(static_cast<std::size_t>(raw.count));
            exact_host_capacity_bytes = saturating_mul(
                static_cast<std::uint64_t>(host_minimizers.capacity()),
                static_cast<std::uint64_t>(sizeof(p_minimizer_type)));
            if ((host_ownership_route
                        == vulkan_sketch_host_ownership_route::
                            fixed_zero_copy_adoption
                    && host_minimizers.size()
                        != host_minimizers.capacity())
                || exact_host_capacity_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || exact_host_capacity_bytes > exact_host_headroom_bytes) {
                throw vulkan_runtime_error{
                    "Vulkan decoded fixed host minimizer capacity exceeded "
                    "admission"};
            }
            if (owned_host_permit) {
                owned_host_permit->shrink_to(exact_host_capacity_bytes);
            }
            runtime.download_slice(
                raw.minimizer_words,
                0u,
                std::as_writable_bytes(std::span<p_minimizer_type>{
                    host_minimizers.data(), host_minimizers.size()}));
        };

        if (host_ownership_route
                == vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
            && exact_host_size_bytes != 0u) {
            auto materialization =
                memory_admission_controller::instance().materialize(
                    *owned_host_permit,
                    [&allocate_and_download,
                     &exact_host_capacity_bytes]() -> bioseq::parallelism::
                            memory_materialization_snapshot {
                        allocate_and_download();
                        return {
                            .actual_allocation_bytes =
                                exact_host_capacity_bytes,
                            .fully_touched_bytes = exact_host_capacity_bytes,
                            .is_host_visible = true,
                            .is_host_coherent = true,
                            .is_process_footprint_reflected = true,
                        };
                    });
            host_permit_released_to_process_footprint =
                materialization.is_permit_released;
            host_footprint_reflected_delta_bytes =
                materialization.reflected_delta_bytes;
            if (!materialization.is_permit_released) {
                // A short result can reuse pages that were already resident, even for a
                // large input with many ambiguous bases. Keep its memory reservation  when
                // the, process-memory measurement cannot account for the new buffer. The
                // caller must keep that reservation until the buffer is freed.
                bioseq_log<logging_level::debug>(
                    "[vulkan sketch] fixed host footprint reconciliation "
                    "retained exact permit: actual={} touched={} before={} "
                    "after={} delta={} permit={}",
                    materialization.allocation_snapshot.actual_allocation_bytes,
                    materialization.allocation_snapshot.fully_touched_bytes,
                    materialization.footprint_before_bytes,
                    materialization.footprint_after_bytes,
                    materialization.reflected_delta_bytes,
                    owned_host_permit->bytes());
            } else {
                owned_host_permit.reset();
            }
        } else {
            allocate_and_download();
            if (host_ownership_route
                    == vulkan_sketch_host_ownership_route::
                        fixed_zero_copy_adoption) {
                // An empty vector needs no reserved buffer bytes or measured memory iacrease.
                if (owned_host_permit) {
                    owned_host_permit->shrink_to(0u);
                }
                owned_host_permit.reset();
                host_permit_released_to_process_footprint = true;
            }
        }

        auto storage = std::make_shared<vulkan_device_minimizer_storage<p_minimizer_type>>();
        storage->words = std::move(raw.minimizer_words);
        if (borrowed_dispatch_permit == nullptr) {
            storage->permit = std::move(*dispatch_permit);
        }
        storage->count = raw.count;
        storage->capacity = max_minimizers;

        return vulkan_sketch_result<p_minimizer_type>{
            .host_permit = std::move(owned_host_permit),
            .device = std::move(storage),
            .host_minimizers = std::move(host_minimizers),
            .dispatch_token = vulkan_sketch_dispatch_token::
                completed_collect_minimizers_roundtrip(),
            .host_ownership_route = host_ownership_route,
            .host_permit_released_to_process_footprint =
                host_permit_released_to_process_footprint,
            .host_footprint_reflected_delta_bytes =
                host_footprint_reflected_delta_bytes,
            .arena_trimmed_before_host_materialization =
                raw.arena_trimmed_before_return,
            .overflow = false,
        };
    } catch (const vulkan_runtime_error&) {
        throw;
    }
}

} // namespace bioseq::gpu::vulkan
