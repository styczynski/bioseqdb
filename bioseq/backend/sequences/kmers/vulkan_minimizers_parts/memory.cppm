/**
 * @file memory.cppm
 * @brief Host and device minimizer ownership and admission accounting.
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

export module bioseq.sequences.kmers.vulkan_minimizers:memory;

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

export namespace bioseq::gpu::vulkan {

/**
 * @brief Retains one reconciled permit across device and host batch storage.
 */
struct vulkan_batch_sketch_memory_retention {
    bioseq::parallelism::memory_permit permit;
};

/**
 * @brief Owns device-resident minimizer words and their admission lifetime.
 * @tparam p_minimizer_type Packed minimizer record represented by `words`.
 */
template <class p_minimizer_type>
struct vulkan_device_minimizer_storage {
    // Declaration order is intentional: reverse destruction frees `words`
    // before either admission owner can release its reservation.
    std::shared_ptr<vulkan_batch_sketch_memory_retention> batch_memory_retention;
    bioseq::parallelism::memory_permit permit;
    buffer words;
    std::uint64_t count = 0;
    std::uint64_t capacity = 0;
    std::uint32_t words32_per_minimizer =
        minimizer_wire_traits<p_minimizer_type>::k_words32_per_minimizer;
    minimizer_word_kind word_kind =
        minimizer_wire_traits<p_minimizer_type>::k_word_kind;
};

/**
 * @brief Borrows device minimizer storage through a shared owner.
 * @tparam p_minimizer_type Packed minimizer record represented by the buffer.
 */
template <class p_minimizer_type>
struct vulkan_device_minimizer_span {
    std::shared_ptr<const vulkan_device_minimizer_storage<p_minimizer_type>> owner{};
    std::uint64_t record_offset = 0u;
    std::uint64_t record_count = 0u;

    /** @brief Returns the device buffer, or null for an empty span. */
    [[nodiscard]] const buffer* words() const noexcept {
        return owner ? &owner->words : nullptr;
    }
    /** @brief Returns the first physical record in the shared device buffer. */
    [[nodiscard]] std::uint64_t offset() const noexcept {
        return record_offset;
    }
    /** @brief Returns the number of resident minimizer records. */
    [[nodiscard]] std::uint64_t count() const noexcept {
        return owner ? record_count : 0u;
    }
    /** @brief Returns the device-word stride of one minimizer. */
    [[nodiscard]] std::uint32_t words32_per_minimizer() const noexcept {
        return owner ? owner->words32_per_minimizer : 0u;
    }
    /** @brief Returns the resident minimizer word encoding. */
    [[nodiscard]] minimizer_word_kind word_kind() const noexcept {
        return owner ? owner->word_kind : minimizer_word_kind::u32;
    }

    /**
     * @brief Borrow a checked record subrange without changing the descriptor offset.
     *
     * The runtime binds the owner's complete buffer and carries `record_offset`
     * in its existing query-record/push wire. This deliberately avoids assuming
     * that a minimizer-record boundary satisfies Vulkan's storage-buffer offset
     * alignment.
     */
    [[nodiscard]] vulkan_device_minimizer_span
    subspan(std::uint64_t first, std::uint64_t count) const {
        if (!owner || record_offset > owner->count || record_count > owner->count - record_offset ||
            first > record_count || count > record_count - first) {
            throw vulkan_runtime_error{"Vulkan minimizer device subspan is out of range"};
        }
        return vulkan_device_minimizer_span{
            .owner = owner,
            .record_offset = record_offset + first,
            .record_count = count,
        };
    }
};

/**
 * @brief Selects ownership and admission rules for host sketch results.
 *
 * The collector-compatible route retains vector growth headroom. The fixed
 * route requires the caller to adopt the returned allocation directly.
 */
enum class vulkan_sketch_host_ownership_route : std::uint8_t {
    collector_compatible = 0,
    fixed_zero_copy_adoption = 1,
};

/** @brief Identifies the runtime route recorded by a sketch dispatch token. */
enum class vulkan_sketch_dispatch_route : std::uint8_t {
    none,
    collect_minimizers,
};

/** @brief Authenticates one completed collector-compatible runtime roundtrip. */
struct vulkan_sketch_dispatch_token {
    vulkan_sketch_dispatch_route route = vulkan_sketch_dispatch_route::none;
    std::uint64_t completed_roundtrips = 0;

    /** @brief Validates the route and exact completed-roundtrip count. */
    [[nodiscard]] constexpr bool is_internally_consistent() const noexcept {
        return route == vulkan_sketch_dispatch_route::collect_minimizers
            && completed_roundtrips == 1;
    }

    /** @brief Creates the token for one completed minimizer collection. */
    [[nodiscard]] static constexpr vulkan_sketch_dispatch_token
    completed_collect_minimizers_roundtrip() noexcept {
        return {
            .route = vulkan_sketch_dispatch_route::collect_minimizers,
            .completed_roundtrips = 1,
        };
    }

    /** @brief Compares route identity and completed-roundtrip count. */
    constexpr bool operator==(
        const vulkan_sketch_dispatch_token&) const noexcept = default;
};

static_assert(!vulkan_sketch_dispatch_token{}.is_internally_consistent());
static_assert(vulkan_sketch_dispatch_token::
    completed_collect_minimizers_roundtrip().is_internally_consistent());

/** @brief Aggregates completed runtime work for the segment-pair route. */
struct vulkan_segment_pair_execution_stats {
    std::uint64_t completed_roundtrips = 0;
    std::uint64_t downloaded_minimizers = 0;
    std::uint64_t gpu_window_splits = 0;
    bool saturated = false;

    /**
     * @brief Adds a counter while preserving overflow as saturation state.
     * @param destination Aggregate counter updated in place.
     * @param increment Counter value to add.
     * @param did_saturate Set when the sum exceeds `uint64_t`.
     */
    static void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment
                > (std::numeric_limits<std::uint64_t>::max)()
                    - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /** @brief Records one completed roundtrip and its downloaded record count. */
    void record_completed_roundtrip(
            std::uint64_t minimizer_count) noexcept {
        add(completed_roundtrips, 1u, saturated);
        add(downloaded_minimizers, minimizer_count, saturated);
    }

    /** @brief Records one capacity-driven GPU window split. */
    void record_gpu_window_split() noexcept {
        add(gpu_window_splits, 1u, saturated);
    }

    /** @brief Merges counters from another segment-pair execution. */
    void merge(const vulkan_segment_pair_execution_stats& other) noexcept {
        saturated = saturated || other.saturated;
        add(completed_roundtrips, other.completed_roundtrips, saturated);
        add(downloaded_minimizers, other.downloaded_minimizers, saturated);
        add(gpu_window_splits, other.gpu_window_splits, saturated);
    }

    /**
     * @brief Validates saturation and optional completed-work requirements.
     * @param require_completed_roundtrip Require at least one roundtrip.
     * @return `true` when the counters satisfy the route contract.
     */
    [[nodiscard]] bool is_internally_consistent(
            bool require_completed_roundtrip = true) const noexcept {
        return !saturated
            && (!require_completed_roundtrip || completed_roundtrips != 0);
    }
};

/** @brief Owns segment pairs together with their exact capacity permit. */
struct vulkan_owned_segment_pairs {
    using value_type = std::pair<std::uint64_t, std::uint64_t>;

    // Reverse destruction releases pair capacity before admission.
    bioseq::parallelism::memory_permit permit;
    std::vector<value_type> pairs;

    /** @brief Constructs an empty pair owner with no admission. */
    vulkan_owned_segment_pairs() = default;
    /** @brief Prevents copying a move-only memory permit. */
    vulkan_owned_segment_pairs(const vulkan_owned_segment_pairs&) = delete;
    /** @brief Prevents copy assignment of a move-only memory permit. */
    vulkan_owned_segment_pairs& operator=(const vulkan_owned_segment_pairs&) = delete;
    /** @brief Transfers vector storage before transferring its permit. */
    vulkan_owned_segment_pairs(vulkan_owned_segment_pairs&& other) noexcept {
        pairs.swap(other.pairs);
        permit = std::move(other.permit);
    }
    /** @brief Releases current capacity and transfers another owned result. */
    vulkan_owned_segment_pairs& operator=(vulkan_owned_segment_pairs&& other) noexcept {
        if (this != &other) {
            bioseq::parallelism::release_admitted_vector_capacity_before_permit(
                pairs, permit);
            pairs.swap(other.pairs);
            permit = std::move(other.permit);
        }
        return *this;
    }

    /** @brief Releases vector capacity before surrendering its permit. */
    void release_capacity_before_permit() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            pairs, permit);
    }

    /** @brief Reports whether the owned pair sequence is empty. */
    [[nodiscard]] bool empty() const noexcept { return pairs.empty(); }
    /** @brief Returns the number of owned pairs. */
    [[nodiscard]] std::size_t size() const noexcept { return pairs.size(); }
    /** @brief Returns a mutable iterator to the first pair. */
    [[nodiscard]] auto begin() noexcept { return pairs.begin(); }
    /** @brief Returns a constant iterator to the first pair. */
    [[nodiscard]] auto begin() const noexcept { return pairs.begin(); }
    /** @brief Returns a mutable iterator past the final pair. */
    [[nodiscard]] auto end() noexcept { return pairs.end(); }
    /** @brief Returns a constant iterator past the final pair. */
    [[nodiscard]] auto end() const noexcept { return pairs.end(); }
    /** @brief Borrows the underlying pair vector. */
    [[nodiscard]] operator const std::vector<value_type>&() const noexcept {
        return pairs;
    }
    /** @brief Tests an owned result against a plain pair vector. */
    friend bool operator==(
        const vulkan_owned_segment_pairs& lhs,
        const std::vector<value_type>& rhs) noexcept {
        return lhs.pairs == rhs;
    }
    /** @brief Tests a plain pair vector against an owned result. */
    friend bool operator==(
        const std::vector<value_type>& lhs,
        const vulkan_owned_segment_pairs& rhs) noexcept {
        return lhs == rhs.pairs;
    }
};

/**
 * @brief Owns host and optional device results from one sketch dispatch.
 * @tparam p_minimizer_type Packed minimizer record.
 */
template <class p_minimizer_type>
struct vulkan_sketch_result {
    // Both possible admission owners precede the host vector so reverse
    // destruction frees vector storage before either reservation is released.
    std::optional<bioseq::parallelism::memory_permit> host_permit;
    std::shared_ptr<vulkan_device_minimizer_storage<p_minimizer_type>> device;
    std::vector<p_minimizer_type> host_minimizers;
    vulkan_sketch_dispatch_token dispatch_token{};
    vulkan_sketch_host_ownership_route host_ownership_route =
        vulkan_sketch_host_ownership_route::collector_compatible;
    bool host_permit_released_to_process_footprint = false;
    std::uint64_t host_footprint_reflected_delta_bytes = 0u;
    bool arena_trimmed_before_host_materialization = false;
    bool overflow = false;

    /** @brief Returns a shared view of device-resident minimizers. */
    [[nodiscard]] vulkan_device_minimizer_span<p_minimizer_type> device_span() const noexcept {
        return vulkan_device_minimizer_span<p_minimizer_type>{
            .owner = device,
            .record_offset = 0u,
            .record_count = device ? device->count : 0u,
        };
    }
};

/**
 * @brief Owns host and device outputs from one batch sketch dispatch.
 * @tparam p_minimizer_type Packed minimizer record.
 */
template <class p_minimizer_type>
struct vulkan_batch_sketch_result {
    // Consumers that detach either host vector without retaining `device`
    // must move `memory_retention` with it.  GPU schedulers naturally retain
    // `device`, which shares the same anchor.
    // It is declared first so reverse destruction releases every accounted
    // vector/device before the final admission anchor.
    std::shared_ptr<vulkan_batch_sketch_memory_retention> memory_retention;
    // The optional authorized readback is a later, independent physical
    // allocation. It is not charged again to the completed sketch reservation.
    bioseq::parallelism::memory_permit host_readback_permit;
    std::vector<p_minimizer_type> host_minimizers;
    std::vector<gpu_seed_query_record_wire> query_records;
    std::shared_ptr<vulkan_device_minimizer_storage<p_minimizer_type>> device;
    runtime_sketch_dispatch_stats dispatch_stats{};
    device_query_sketch_authorization query_authority{};
    vulkan_sketch_host_materialization materialization =
        vulkan_sketch_host_materialization::host_and_device;
    std::uint64_t device_minimizer_count = 0;
    bool overflow = false;
    bool host_readback_completed = false;

    vulkan_batch_sketch_result() = default;
    explicit vulkan_batch_sketch_result(
        vulkan_sketch_host_materialization selected_materialization) noexcept
        : materialization(selected_materialization) {}
    vulkan_batch_sketch_result(
        std::shared_ptr<vulkan_batch_sketch_memory_retention> retained_memory,
        vulkan_sketch_host_materialization selected_materialization) noexcept
        : memory_retention(std::move(retained_memory)),
          materialization(selected_materialization) {}

    vulkan_batch_sketch_result(const vulkan_batch_sketch_result&) = delete;
    vulkan_batch_sketch_result& operator=(const vulkan_batch_sketch_result&) = delete;

    /// Move physical allocations before moving the permits that authorize them.
    vulkan_batch_sketch_result(vulkan_batch_sketch_result&& other) noexcept {
        adopt_empty(other);
    }

    vulkan_batch_sketch_result& operator=(vulkan_batch_sketch_result&& other) noexcept {
        if (this == std::addressof(other)) {
            return *this;
        }
        query_authority = {};
        device.reset();
        {
            std::vector<gpu_seed_query_record_wire> empty_records;
            query_records.swap(empty_records);
        }
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            host_minimizers, host_readback_permit);
        memory_retention.reset();
        adopt_empty(other);
        return *this;
    }

    /** @brief Returns a shared view of device-resident minimizers. */
    [[nodiscard]] vulkan_device_minimizer_span<p_minimizer_type> device_span() const noexcept {
        return vulkan_device_minimizer_span<p_minimizer_type>{
            .owner = device,
            .record_offset = 0u,
            .record_count = device ? device->count : 0u,
        };
    }

private:
    /// The destination has no physical storage or admission when called.
    void adopt_empty(vulkan_batch_sketch_result& other) noexcept {
        host_minimizers.swap(other.host_minimizers);
        query_records.swap(other.query_records);
        device = std::move(other.device);
        query_authority = std::move(other.query_authority);
        dispatch_stats = std::exchange(other.dispatch_stats, {});
        materialization = other.materialization;
        device_minimizer_count = std::exchange(other.device_minimizer_count, 0u);
        overflow = std::exchange(other.overflow, false);
        host_readback_completed = std::exchange(other.host_readback_completed, false);
        host_readback_permit = std::move(other.host_readback_permit);
        memory_retention = std::move(other.memory_retention);
    }
};

/**
 * @brief Decodes downloaded bytes into exact minimizer records.
 * @tparam p_minimizer_type Trivially copied packed minimizer record.
 * @param bytes Downloaded device bytes.
 * @param count Number of complete minimizer records.
 * @return Records in device emission order.
 * @throws vulkan_runtime_error If the count or byte span is invalid.
 */
template <class p_minimizer_type>
[[nodiscard]] std::vector<p_minimizer_type> decode_host_minimizer_bytes(
    std::span<const std::byte> bytes, std::uint64_t count)
{
    if (count > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)() / sizeof(p_minimizer_type))) {
        throw vulkan_runtime_error{"Vulkan minimizer count exceeds host address space"};
    }
    const std::size_t n = static_cast<std::size_t>(count);
    const std::size_t expected = n * sizeof(p_minimizer_type);
    if (bytes.size() < expected) {
        throw vulkan_runtime_error{"Vulkan minimizer download was truncated"};
    }
    std::vector<p_minimizer_type> out(n);
    if (expected != 0u) {
        std::memcpy(out.data(), bytes.data(), expected);
    }
    return out;
}

/**
 * @brief Estimates output capacity for one sketch dispatch.
 * @param bp_size Biological input length.
 * @param window_size Window length in k-mers.
 * @return Saturating record-capacity estimate.
 */
[[nodiscard]] inline std::uint64_t estimate_vulkan_minimizer_capacity(
    std::uint64_t bp_size, std::uint64_t window_size) noexcept
{
    using bioseq::common::saturating_mul;
    // ~2/(w+1) minimizers per bp in practice; 3·(bp/w) is a safe (>1.5×) upper
    // planning estimate that keeps ordinary single-pass sketch buffers small.
    // Those APIs fail closed if it underestimates. Target-index construction is
    // different: it first obtains exact T without an output allocation, records
    // whether this estimate was low, then sizes and admits the deterministic
    // emit from T rather than treating this heuristic as a semantic cap.
    if (bp_size <= k_default_sketch_tile_bp) {
        return std::max<std::uint64_t>(64u, bp_size);
    }
    const std::uint64_t base = window_size == 0 ? bp_size : (bp_size / window_size + 64u);
    return std::max<std::uint64_t>(64u, saturating_mul(base, 3u));
}

/**
 * @brief Computes resident device bytes for a minimizer capacity.
 * @tparam p_minimizer_type Packed minimizer record.
 * @param max_minimizers Record capacity.
 * @return Saturating device byte count.
 */
template <class p_minimizer_type>
[[nodiscard]] std::uint64_t vulkan_sketch_resident_bytes(
    std::uint64_t max_minimizers) noexcept
{
    return minimizer_wire_traits<p_minimizer_type>::device_bytes_for_count(max_minimizers);
}

/**
 * @brief Computes transient bytes reserved for a sketch dispatch.
 * @param bp Biological input length.
 * @param sequence_bytes Packed input bytes before upload alignment.
 * @param letters_per_unit Biological symbols stored in one packed unit.
 * @param window_size Window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @param max_minimizers Output record capacity.
 * @param minimizer_device_output_bytes Resident output bytes.
 * @return Saturating transient byte count, or `UINT64_MAX` for an invalid plan.
 */
[[nodiscard]] inline std::uint64_t vulkan_sketch_transient_reserved_bytes(
    std::uint64_t bp,
    std::uint64_t sequence_bytes,
    std::uint32_t letters_per_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    std::uint64_t max_minimizers,
    std::uint64_t minimizer_device_output_bytes)
{
    const gpu_sketch_memory_plan plan = make_gpu_sketch_memory_plan(
        bp,
        aligned_upload_bytes(sequence_bytes),
        sizeof(vulkan_rank_lut),
        max_minimizers,
        minimizer_device_output_bytes,
        gpu_sketch_purpose::minimizer_words,
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        letters_per_unit);
    if (!plan.is_valid) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return bioseq::common::saturating_add(
        plan.arena_scaffold_bytes,
        plan.count_submit_host_transient_bytes);
}

/** @brief Bounded allocator allowance for one fixed host result allocation. */
inline constexpr std::uint64_t k_vulkan_exact_host_allocation_headroom_bytes =
    64ull * 1024ull;

/**
 * @brief Adds bounded allocator overhead to one fixed host allocation.
 * @param requested_bytes Exact payload bytes.
 * @return Saturating admission byte count.
 */
[[nodiscard]] constexpr std::uint64_t
vulkan_exact_host_allocation_admission_bytes(
    std::uint64_t requested_bytes) noexcept
{
    using bioseq::common::saturating_add;
    return saturating_add(
        requested_bytes,
        k_vulkan_exact_host_allocation_headroom_bytes);
}

/**
 * @brief Computes admission for a fixed host result allocation.
 * @param exact_host_size_bytes Exact result payload bytes.
 * @return Zero for an empty result, otherwise payload plus allocator overhead.
 */
[[nodiscard]] constexpr std::uint64_t
vulkan_fixed_host_result_admission_bytes(
        std::uint64_t exact_host_size_bytes) noexcept {
    return exact_host_size_bytes == 0u
        ? 0u
        : vulkan_exact_host_allocation_admission_bytes(
            exact_host_size_bytes);
}

/**
 * @brief Sums all retained resources required during dispatch.
 * @param physical_arena_bytes Physical Vulkan arena allocation.
 * @param host_transient_headroom_bytes Host dispatch staging with overhead.
 * @param query_gate_bytes Query-gate payload bytes.
 * @param resident_allocation_bytes Pessimistic resident result allocation.
 * @return Saturating dispatch admission byte count.
 */
[[nodiscard]] constexpr std::uint64_t
vulkan_sketch_dispatch_admission_bytes(
    std::uint64_t physical_arena_bytes,
    std::uint64_t host_transient_headroom_bytes,
    std::uint64_t query_gate_bytes,
    std::uint64_t resident_allocation_bytes) noexcept
{
    using bioseq::common::saturating_add;
    return saturating_add(
        resident_allocation_bytes,
        saturating_add(
            physical_arena_bytes,
            saturating_add(
                host_transient_headroom_bytes,
                query_gate_bytes)));
}

/**
 * @brief Computes retained admission during exact host materialization.
 * @param actual_device_allocation_bytes Actual resident device allocation.
 * @param exact_host_headroom_bytes Exact host allocation with overhead.
 * @return Saturating retained byte count.
 */
[[nodiscard]] constexpr std::uint64_t
vulkan_sketch_host_materialization_admission_bytes(
    std::uint64_t actual_device_allocation_bytes,
    std::uint64_t exact_host_headroom_bytes) noexcept
{
    return bioseq::common::saturating_add(
        actual_device_allocation_bytes,
        exact_host_headroom_bytes);
}

/** @brief Estimates retained resources used to admit and split one GPU batch. */
struct vulkan_sketch_memory_estimate {
    std::uint64_t packed_sequence_bytes = 0;
    std::uint64_t query_minimizer_bytes = 0;
    std::uint64_t target_minimizer_bytes = 0;
    std::uint64_t index_slot_bytes = 0;
    std::uint64_t index_position_bytes = 0;
    std::uint64_t anchor_bytes = 0;
    std::uint64_t sort_scratch_bytes = 0;
    std::uint64_t chain_bytes = 0;

    /** @brief Returns the saturating sum of every estimated resource. */
    [[nodiscard]] std::uint64_t total() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t sum = 0;
        sum = saturating_add(sum, packed_sequence_bytes);
        sum = saturating_add(sum, query_minimizer_bytes);
        sum = saturating_add(sum, target_minimizer_bytes);
        sum = saturating_add(sum, index_slot_bytes);
        sum = saturating_add(sum, index_position_bytes);
        sum = saturating_add(sum, anchor_bytes);
        sum = saturating_add(sum, sort_scratch_bytes);
        sum = saturating_add(sum, chain_bytes);
        return sum;
    }
};

} // namespace bioseq::gpu::vulkan
