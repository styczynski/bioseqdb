/**
 * @file directory.cppm
 * @brief Loads grouped index directories within a memory budget and decodes their records.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:prepared_reader_directory;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader_receipts;
import bioseq.chaining.core;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.common.seqan_adapter;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.instrumentation;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// One hash and its range [position_begin, position_end) in an extent's position array.
struct grouped_soa_decoded_control {
    /// Minimizer hash represented by this control.
    std::uint64_t hash = 0;
    /// First position-array index for this hash.
    std::uint64_t position_begin = 0;
    /// Position-array index just past this hash's last position.
    std::uint64_t position_end = 0;
    /// Occurrence count capped at the persisted profile limit plus one.
    std::uint64_t saturated_count = 0;

    [[nodiscard]] bool operator==(const grouped_soa_decoded_control&) const = default;
};

/// Identifies the step that failed while reserving or allocating reader memory.
/// It adds detail to minimizer_generation_encoding_error::admission.
enum class grouped_soa_query_admission_stage : std::uint8_t {
    none,
    identity_and_directory_reservation,
    query_hash_reservation,
    query_hash_allocation,
    query_hash_reconcile,
    query_route_reservation,
    query_route_allocation,
    query_route_reconcile,
    control_payload_reservation,
    control_payload_allocation,
    control_payload_reconcile,
    control_decode_reservation,
    control_decode_allocation,
    control_decode_reconcile,
    position_image_reservation,
    position_image_allocation,
    position_image_reconcile,
};

/// Return the diagnostic name of a memory-allocation stage.
[[nodiscard]] constexpr std::string_view
grouped_soa_query_admission_substage_name(grouped_soa_query_admission_stage substage) noexcept {
    switch (substage) {
    case grouped_soa_query_admission_stage::none:
        return "none";
    case grouped_soa_query_admission_stage::identity_and_directory_reservation:
        return "identity_and_directory_reservation";
    case grouped_soa_query_admission_stage::query_hash_reservation:
        return "query_hash_reservation";
    case grouped_soa_query_admission_stage::query_hash_allocation:
        return "query_hash_allocation";
    case grouped_soa_query_admission_stage::query_hash_reconcile:
        return "query_hash_reconcile";
    case grouped_soa_query_admission_stage::query_route_reservation:
        return "query_route_reservation";
    case grouped_soa_query_admission_stage::query_route_allocation:
        return "query_route_allocation";
    case grouped_soa_query_admission_stage::query_route_reconcile:
        return "query_route_reconcile";
    case grouped_soa_query_admission_stage::control_payload_reservation:
        return "control_payload_reservation";
    case grouped_soa_query_admission_stage::control_payload_allocation:
        return "control_payload_allocation";
    case grouped_soa_query_admission_stage::control_payload_reconcile:
        return "control_payload_reconcile";
    case grouped_soa_query_admission_stage::control_decode_reservation:
        return "control_decode_reservation";
    case grouped_soa_query_admission_stage::control_decode_allocation:
        return "control_decode_allocation";
    case grouped_soa_query_admission_stage::control_decode_reconcile:
        return "control_decode_reconcile";
    case grouped_soa_query_admission_stage::position_image_reservation:
        return "position_image_reservation";
    case grouped_soa_query_admission_stage::position_image_allocation:
        return "position_image_allocation";
    case grouped_soa_query_admission_stage::position_image_reconcile:
        return "position_image_reconcile";
    }
    return "unknown";
}

/// Memory statistics saved before a failed reader frees its buffers. Capacity
/// fields describe allocations at the failure. Controller fields use its
/// rejection snapshot when available, or a diagnostic snapshot taken afterward.
struct grouped_soa_query_admission_stats {
    grouped_soa_query_admission_stage substage = grouped_soa_query_admission_stage::none;
    bioseq::parallelism::memory_admission_failure_reason controller_reason =
        bioseq::parallelism::memory_admission_failure_reason::none;
    std::uint64_t retained_position_count = 0;
    std::uint64_t retained_position_bytes = 0;
    std::uint64_t allocation_headroom_bytes = 0;
    std::uint64_t projected_bytes = 0;
    std::uint64_t requested_growth_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    std::uint64_t permit_bytes = 0;
    std::uint64_t controller_reserved_bytes = 0;
    std::uint64_t controller_budget_ceiling_bytes = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t controller_allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t process_footprint_bytes = 0;
    std::uint64_t system_available_bytes = 0;
    std::uint64_t system_total_bytes = 0;
    std::uint64_t process_soft_cap_bytes = 0;
    std::uint64_t query_record_count = 0;
    std::uint64_t query_record_capacity = 0;
    std::uint64_t query_hash_count = 0;
    std::uint64_t query_hash_capacity = 0;
    std::uint64_t selected_extent_count = 0;
    std::uint64_t selected_extent_capacity = 0;
    std::uint64_t selected_control_capacity_bytes = 0;
    std::uint64_t control_payload_capacity_bytes = 0;
    std::uint64_t releasable_protocol_query_hash_bytes = 0;
    bool controller_failure_snapshot_present = false;
    bool injected_failure = false;
};

/// Lets tests force one memory failure at a chosen step. Production callers
/// leave fail_admission_substage at none.
struct grouped_soa_query_reader_options {
    /// Step at which a test should force a memory failure.
    grouped_soa_query_admission_stage fail_admission_substage =
        grouped_soa_query_admission_stage::none;
};

/// Memory required for the final array of query positions.
struct grouped_soa_position_image_plan {
    /// Number of decoded positions to store.
    std::uint64_t retained_position_count = 0;
    /// Exact bytes required by the final position array.
    std::uint64_t retained_position_bytes = 0;
    /// Extra memory reserved during allocation, beyond the final array bytes.
    std::uint64_t allocation_headroom_bytes = 0;
    /// Bytes already allocated before creating the position array.
    std::uint64_t owned_capacity_bytes = 0;
    /// Reservation required for the existing buffers and new position array.
    std::uint64_t projected_bytes = 0;
};

/// Tracks the reader's allocated buffers and their total byte count.
/// Each allocation change updates the total directly, so memory reservations
/// do not need to scan every buffer.
struct protocol_owned_bytes_ledger {
    /// Encoded and decoded directory bytes.
    std::uint64_t directory_bytes = 0;
    /// Bytes used by full query hashes or compact hash suffixes.
    std::uint64_t query_hash_bytes = 0;
    /// Bytes used by query counts, position offsets and write counts.
    std::uint64_t query_record_bytes = 0;
    /// Bytes used by the offsets into hash-prefix buckets.
    std::uint64_t radix_offset_bytes = 0;
    /// Bytes in the final array of global positions.
    std::uint64_t global_position_bytes = 0;
    /// Bytes used to map directory entries to selected extents.
    std::uint64_t ordinal_map_bytes = 0;
    /// Selected-extent vector bytes.
    std::uint64_t selected_extent_vector_bytes = 0;
    /// Bytes used by control boundaries and query routes inside selected extents.
    std::uint64_t selected_extent_owner_bytes = 0;
    /// Bytes in positions stored separately for individual queries.
    std::uint64_t query_record_position_bytes = 0;
    /// Bytes reserved in the temporary control decode buffer.
    std::uint64_t control_scratch_bytes = 0;
    /// Bytes recorded for an in-progress row session.
    std::uint64_t active_session_bytes = 0;
    /// Cached sum of the allocation sizes above.
    std::uint64_t total_bytes = 0;

    protocol_owned_bytes_ledger() = default;
    protocol_owned_bytes_ledger(const protocol_owned_bytes_ledger&) = default;
    protocol_owned_bytes_ledger& operator=(const protocol_owned_bytes_ledger&) = default;

    /// Transfer the ledger and clear the source accounting state.
    protocol_owned_bytes_ledger(protocol_owned_bytes_ledger&& other) noexcept
        : protocol_owned_bytes_ledger(static_cast<const protocol_owned_bytes_ledger&>(other)) {
        other.reset();
    }

    /// Replace this ledger by transfer and clear the source accounting state.
    protocol_owned_bytes_ledger& operator=(protocol_owned_bytes_ledger&& other) noexcept {
        if (this != &other) {
            *this = static_cast<const protocol_owned_bytes_ledger&>(other);
            other.reset();
        }
        return *this;
    }

    /// Replace an allocation's contribution to the total, checking for overflow.
    /// The caller updates the corresponding field after this succeeds.
    [[nodiscard]] bool replace_checked(std::uint64_t old_bytes, std::uint64_t new_bytes) noexcept {
        if (old_bytes > total_bytes)
            return false;
        const std::uint64_t without_old = total_bytes - old_bytes;
        if (new_bytes > (std::numeric_limits<std::uint64_t>::max)() - without_old) {
            return false;
        }
        total_bytes = without_old + new_bytes;
        return true;
    }

    /// Check whether temporary extra bytes can be added to the total without overflow.
    [[nodiscard]] bool add_temporary_peak_checked(std::uint64_t peak_bytes) const noexcept {
        return peak_bytes <= (std::numeric_limits<std::uint64_t>::max)() - total_bytes;
    }

    /// Check that all allocation sizes add up to the cached total without overflow.
    [[nodiscard]] bool field_sum_holds() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t sum = directory_bytes;
        sum = saturating_add(sum, query_hash_bytes);
        sum = saturating_add(sum, query_record_bytes);
        sum = saturating_add(sum, radix_offset_bytes);
        sum = saturating_add(sum, global_position_bytes);
        sum = saturating_add(sum, ordinal_map_bytes);
        sum = saturating_add(sum, selected_extent_vector_bytes);
        sum = saturating_add(sum, selected_extent_owner_bytes);
        sum = saturating_add(sum, query_record_position_bytes);
        sum = saturating_add(sum, control_scratch_bytes);
        sum = saturating_add(sum, active_session_bytes);
        return sum != (std::numeric_limits<std::uint64_t>::max)() && sum == total_bytes;
    }

    /// Clear all recorded byte counts. This does not free the buffers they describe.
    void reset() noexcept {
        directory_bytes = 0;
        query_hash_bytes = 0;
        query_record_bytes = 0;
        radix_offset_bytes = 0;
        global_position_bytes = 0;
        ordinal_map_bytes = 0;
        selected_extent_vector_bytes = 0;
        selected_extent_owner_bytes = 0;
        query_record_position_bytes = 0;
        control_scratch_bytes = 0;
        active_session_bytes = 0;
        total_bytes = 0;
    }
};

/// Calculate the memory needed for query positions without allocating it.
/// Reject size calculations that overflow.
[[nodiscard]] inline std::expected<grouped_soa_position_image_plan,
                                   minimizer_generation_encoding_error>
plan_grouped_soa_position_image(std::uint64_t retained_position_count,
                                std::uint64_t owned_capacity_bytes) noexcept {
    const std::uint64_t retained_position_bytes =
        bioseq::common::saturating_mul(retained_position_count, sizeof(std::uint64_t));
    // The final position count is known, so this array needs no growth allowance.
    // Report zero extra bytes in the allocation statistics.
    constexpr std::uint64_t allocation_headroom_bytes = 0u;
    const std::uint64_t projected_bytes =
        bioseq::common::saturating_add(owned_capacity_bytes, retained_position_bytes);
    if (retained_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        projected_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return grouped_soa_position_image_plan{
        .retained_position_count = retained_position_count,
        .retained_position_bytes = retained_position_bytes,
        .allocation_headroom_bytes = allocation_headroom_bytes,
        .owned_capacity_bytes = owned_capacity_bytes,
        .projected_bytes = projected_bytes,
    };
}
namespace minimizer_prepared_reader_detail {

/// Selects decoded types and read order for a saved storage layout.
/// @tparam p_layout Saved index layout.
template <minimizer_generation_layout p_layout> struct codec_traits;

/// Grouped-SoA reads every control extent before reading positions.
template <> struct codec_traits<minimizer_generation_layout::grouped_soa_segment_local> {
    static constexpr minimizer_generation_layout layout =
        minimizer_generation_layout::grouped_soa_segment_local;
    using control_type = grouped_soa_decoded_control;
    using position_type = std::uint64_t;
    using hash_type = std::uint64_t;
    static constexpr bool global_controls_then_positions = true;
};

/// Owns checked generation metadata, a copied directory and its memory reservation.
/// Every supplied record must use the layout selected by p_traits.
/// @tparam p_traits Layout traits used for validation and reading.
template <class p_traits> class admitted_prepared_reader_core {
  protected:
    bioseq::parallelism::memory_permit permit_{};
    grouped_soa_directory_view directory_{};
    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    expected_minimizer_generation_identity expected_{};
    grouped_soa_minimizer_profile profile_{
        .layout = minimizer_generation_layout::invalid,
    };

    admitted_prepared_reader_core() = default;
    admitted_prepared_reader_core(admitted_prepared_reader_core&&) noexcept = default;
    admitted_prepared_reader_core& operator=(admitted_prepared_reader_core&&) noexcept = default;
    admitted_prepared_reader_core(const admitted_prepared_reader_core&) = delete;
    admitted_prepared_reader_core& operator=(const admitted_prepared_reader_core&) = delete;

    /// Check the metadata and copy the directory after reserving its memory.
    /// The destination must be fresh. Any failure leaves it unchanged and frees
    /// temporary storage before releasing the reservation.
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    begin_admitted_core(
        admitted_prepared_reader_core& destination,
        bioseq::parallelism::memory_admission_controller& admission,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const expected_minimizer_generation_identity& expected,
        const grouped_soa_minimizer_profile& profile,
        std::span<const std::byte> encoded_directory,
        std::uint64_t additional_logical_bytes = 0,
        bioseq::parallelism::memory_admission_failure* admission_failure = nullptr) {
        const bool destination_is_fresh =
            !destination.permit_ && destination.directory_.owned_capacity_bytes() == 0u &&
            destination.head_.layout == minimizer_generation_layout::invalid &&
            destination.manifest_.layout == minimizer_generation_layout::invalid &&
            destination.expected_.layout == minimizer_generation_layout::invalid &&
            destination.profile_.layout == minimizer_generation_layout::invalid;
        DEBUG_ASSERT(destination_is_fresh);
        if (!destination_is_fresh) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (head.layout != p_traits::layout || manifest.layout != p_traits::layout ||
            expected.layout != p_traits::layout || profile.layout != p_traits::layout ||
            !validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (auto validated = validate_minimizer_generation_storage_contract(manifest, expected);
            !validated) {
            return std::unexpected(validated.error());
        }
        const std::size_t stride = minimizer_directory_entry_encoded_bytes(profile);
        if (encoded_directory.size() % stride != 0) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        const std::uint64_t entry_count = encoded_directory.size() / stride;
        std::uint64_t logical_bytes = bioseq::common::saturating_add(
            encoded_directory.size(),
            bioseq::common::saturating_mul(entry_count,
                                           sizeof(grouped_soa_bucket_directory_entry)));
        logical_bytes = bioseq::common::saturating_add(logical_bytes, additional_logical_bytes);
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(logical_bytes, admission_failure);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        auto directory = grouped_soa_directory_view::decode_admitted(
            encoded_directory, profile, *permit, admission_failure);
        if (!directory)
            return std::unexpected(directory.error());
        if (auto valid = validate_minimizer_generation_metadata(head, manifest, *directory);
            !valid) {
            return std::unexpected(valid.error());
        }
        // Install the directory only after every check succeeds. On failure, the
        // local directory is freed before its memory reservation is released.
        destination.permit_ = std::move(*permit);
        destination.directory_ = std::move(*directory);
        destination.head_ = head;
        destination.manifest_ = manifest;
        destination.expected_ = expected;
        destination.profile_ = profile;
        return {};
    }

    /// Return bytes owned by the encoded and decoded directory.
    [[nodiscard]] std::uint64_t core_owned_capacity_bytes() const noexcept {
        return directory_.owned_capacity_bytes();
    }

    /// Release the directory before returning its reservation to the controller.
    void release_core_noexcept() noexcept {
        directory_ = grouped_soa_directory_view{};
        permit_.release();
    }
};

} // namespace minimizer_prepared_reader_detail

/// Decode a complete control extent and check hash order, position ranges and
/// saturated counts against the directory. Alignment padding must be zero.
[[nodiscard]] inline std::expected<std::vector<grouped_soa_decoded_control>,
                                   minimizer_generation_encoding_error>
decode_grouped_soa_controls(std::span<const std::byte> input,
                            const grouped_soa_bucket_directory_entry& entry,
                            const grouped_soa_minimizer_profile& profile) {
    if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        entry.control_encoded_bytes != input.size()) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                       profile.hash_width,
                                                       entry.shape_offset_width,
                                                       profile.count_width);
    if (!layout || layout->encoded_bytes != input.size() ||
        entry.control_element_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t n = static_cast<std::size_t>(entry.control_element_count);
    const std::size_t h = static_cast<std::uint8_t>(profile.hash_width);
    const std::size_t o = static_cast<std::uint8_t>(entry.shape_offset_width);
    const std::size_t c = static_cast<std::uint8_t>(profile.count_width);
    const std::size_t hash_end = n * h;
    const std::size_t prefix_begin = static_cast<std::size_t>(layout->prefix_offset);
    const std::size_t prefix_end = prefix_begin + (n + 1u) * o;
    const std::size_t count_begin = static_cast<std::size_t>(layout->count_offset);
    const std::size_t count_end = count_begin + n * c;
    if (!minimizer_generation_wire_detail::all_zero(input, hash_end, prefix_begin) ||
        !minimizer_generation_wire_detail::all_zero(input, prefix_end, count_begin) ||
        !minimizer_generation_wire_detail::all_zero(input, count_end, input.size())) {
        return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
    }
    std::vector<grouped_soa_decoded_control> controls;
    controls.reserve(n);
    std::uint64_t previous_hash = 0;
    std::uint64_t previous_prefix = 0;
    for (std::size_t i = 0; i <= n; ++i) {
        const auto prefix = minimizer_generation_wire_detail::read_shape_scalar(
            input, prefix_begin + i * o, entry.shape_offset_width);
        if (!prefix || (i == 0 && *prefix != 0) || (i != 0 && *prefix <= previous_prefix) ||
            *prefix > entry.position_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        if (i == n) {
            if (*prefix != entry.position_element_count) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            break;
        }
        const auto hash =
            minimizer_generation_wire_detail::read_shape_scalar(input, i * h, profile.hash_width);
        const auto next_prefix = minimizer_generation_wire_detail::read_shape_scalar(
            input, prefix_begin + (i + 1u) * o, entry.shape_offset_width);
        const auto count = minimizer_generation_wire_detail::read_shape_scalar(
            input, count_begin + i * c, profile.count_width);
        if (!hash || *hash > profile.max_hash || (i != 0 && *hash <= previous_hash)) {
            return std::unexpected(minimizer_generation_encoding_error::control_order);
        }
        if (!next_prefix || !count || *next_prefix <= *prefix ||
            *next_prefix > entry.position_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        const std::uint64_t delta = *next_prefix - *prefix;
        if (*count != std::min(delta, profile.max_occ_cap + 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        controls.push_back(grouped_soa_decoded_control{
            .hash = *hash,
            .position_begin = *prefix,
            .position_end = *next_prefix,
            .saturated_count = *count,
        });
        previous_hash = *hash;
        previous_prefix = *prefix;
    }
    if (controls.empty() || controls.front().hash != entry.lo_hash ||
        controls.back().hash != entry.hi_hash) {
        return std::unexpected(minimizer_generation_encoding_error::hash_range);
    }
    return controls;
}

/// Decode segment-local positions as global `(bp << 1) | strand` values.
/// Check that encoded positions increase strictly within each hash's range.
/// The caller supplies validated controls and a valid position width.
[[nodiscard]] inline std::expected<std::vector<std::uint64_t>, minimizer_generation_encoding_error>
decode_grouped_soa_positions(std::span<const std::byte> input,
                             std::span<const grouped_soa_decoded_control> controls,
                             const grouped_soa_bucket_directory_entry& entry,
                             const minimizer_position_segment_bounds& core,
                             const grouped_soa_minimizer_profile& profile) {
    if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        input.size() != entry.position_encoded_bytes ||
        entry.position_element_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t p = static_cast<std::uint8_t>(profile.position_width);
    if (entry.position_element_count > (std::numeric_limits<std::size_t>::max)() / p ||
        input.size() != static_cast<std::size_t>(entry.position_element_count) * p) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    std::vector<std::uint64_t> positions;
    positions.reserve(static_cast<std::size_t>(entry.position_element_count));
    for (std::size_t i = 0; i < entry.position_element_count; ++i) {
        const auto local = minimizer_generation_wire_detail::read_shape_scalar(
            input, i * p, profile.position_width);
        if (!local) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t local_bp = *local >> 1u;
        if (local_bp >= core.end_bp - core.begin_bp ||
            core.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t global_bp = core.begin_bp + local_bp;
        if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        positions.push_back((global_bp << 1u) | (*local & 1u));
    }
    for (const auto& control : controls) {
        std::optional<std::uint64_t> previous;
        for (std::uint64_t i = control.position_begin; i < control.position_end; ++i) {
            const std::uint64_t value = positions[static_cast<std::size_t>(i)];
            if (previous && value <= *previous) {
                return std::unexpected(minimizer_generation_encoding_error::position_range);
            }
            previous = value;
        }
    }
    return positions;
}

} // namespace bioseq::indexing
