/**
 * @file cache.cppm
 * @brief Batch-scoped GPU minimizer-index cache and admission boundary.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fmt/format.h>
#include <libassert/assert.hpp>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>

export module bioseq.indexing.gpu_minimizer_index:cache;

import :direct_ingest;
import :hash_sorted_ingest;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Batch/wave-scoped GPU minimizer index cache. Owned by the outer query
/// batch/segment wave. It uploads each distinct index once, reuses the resident
/// handle across queries that share it, and evicts LRU when the resident byte
/// budget is exceeded. Public cache operations serialize through an internal
/// mutex, while returned resident handles retain their own device allocations.
class gpu_minimizer_index_cache {
public:
    /// Create an empty cache with a resident allocation limit in bytes.
    /// A zero limit disables byte-budget eviction.
    explicit gpu_minimizer_index_cache(std::uint64_t resident_limit_bytes)
        : resident_limit_bytes_(resident_limit_bytes) {}

    /// Failures that prevent binding one canonical gate identity to the cache.
    enum class gate_bind_error : std::uint8_t {
        cache_not_empty,
        invalid_identity,
        identity_mismatch,
    };
    /// Failures that prevent adoption of an externally built resident index.
    enum class built_insert_error : std::uint8_t {
        null_handle,
        resident_limit,
        gate_identity_mismatch,
        resident_key_mismatch,
        duplicate_direct_key,
    };
    /// Failures from lookup of an operation-owned preinserted resident.
    enum class required_preinsert_lookup_error : std::uint8_t {
        invalid_key,
        miss,
        resident_mismatch,
    };

    /// Canonical gate words and digest metadata shared by one query wave.
    struct gate_identity {
        bool enabled = false;
        std::uint32_t word_count = 0;
        std::uint32_t mask = 0;
        std::uint64_t digest_lo = 0;
        std::uint64_t digest_hi = 0;
        // The digest is only a prefilter.  The immutable canonical words are
        // the cache-identity authority, so even a forced 128-bit collision
        // cannot publish or retrieve an index built for another query wave.
        std::shared_ptr<const std::vector<std::uint32_t>> canonical_words{};

        /// Project cache-owned gate state into the runtime identity type.
        [[nodiscard]] bioseq::gpu::vulkan::gpu_index_gate_identity
        runtime_identity() const noexcept {
            return bioseq::gpu::vulkan::gpu_index_gate_identity{
                .enabled = enabled,
                .word_count = word_count,
                .mask = mask,
                .digest_lo = digest_lo,
                .digest_hi = digest_hi,
                .canonical_words = canonical_words,
            };
        }

        /// Check structural consistency between enabled state, metadata, and canonical words.
        [[nodiscard]] bool is_well_formed() const noexcept {
            return runtime_identity().is_well_formed();
        }

        /// Compare this canonical identity with gate metadata carried by a cache key.
        [[nodiscard]] bool metadata_matches(
            const gpu_minimizer_index_cache_key& key) const noexcept
        {
            return runtime_identity().is_exactly_equal_to(
                gpu_index_gate_identity_from_key(key));
        }

        /// Compare gate metadata and, when enabled, the authoritative canonical words.
        [[nodiscard]] bool operator==(const gate_identity& other) const {
            if (enabled != other.enabled || word_count != other.word_count
                || mask != other.mask || digest_lo != other.digest_lo
                || digest_hi != other.digest_hi) {
                return false;
            }
            if (!enabled) return true;
            if (!canonical_words || !other.canonical_words) return false;
            if (canonical_words.get() == other.canonical_words.get()) return true;
            return *canonical_words == *other.canonical_words;
        }
    };

    /// Bind the empty cache to one gate identity for all subsequent operations.
    /// Rebinding the identical identity is idempotent.
    [[nodiscard]] std::expected<void, gate_bind_error> bind_gate_once(
            gate_identity identity) {
        std::lock_guard<std::mutex> guard(mu_);
        if (!identity.is_well_formed()) {
            return std::unexpected{gate_bind_error::invalid_identity};
        }
        if (gate_binding_) {
            return *gate_binding_ == identity
                ? std::expected<void, gate_bind_error>{}
                : std::unexpected{gate_bind_error::identity_mismatch};
        }
        if (!entries_.empty()) {
            return std::unexpected{gate_bind_error::cache_not_empty};
        }
        gate_binding_ = identity;
        return {};
    }

    /// Reuse or materialize a resident GPU index for a CPU fragment index.
    ///
    /// The returned shared handle owns the device buffers independently of cache eviction.
    /// When `decline_out` is supplied, expected capacity or admission failures return null and
    /// populate the decline record. Contract violations still throw a Vulkan runtime error.
    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> get_or_upload(
        bioseq::gpu::vulkan::runtime& runtime,
        const minimizer_hashmap_fragment_index& idx,
        const gpu_minimizer_index_cache_key& key,
        bool cache_enabled,
        gpu_index_upload_decline* decline_out = nullptr)
    {
        if (decline_out != nullptr) {
            *decline_out = gpu_index_upload_decline{};
        }
        if (key.source_direct_sorted_dense) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "source-direct sorted-dense keys require preinsert lookup");
        }
        std::lock_guard<std::mutex> guard(mu_);
        validate_key_gate_locked(key);
        if (cache_enabled) {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                touch_lru_locked(key);
                return found->second;
            }
        }

        std::shared_ptr<const gpu_resident_minimizer_index> handle;
        try {
            handle = upload_locked_index(runtime, idx, key, decline_out);
        } catch (const gpu_index_too_large&) {
            if (decline_out != nullptr) {
                decline_out->reason =
                    gpu_index_upload_decline_reason::device_abi_overflow;
                return nullptr;
            }
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan minimizer index exceeds the 32-bit device ABI");
        } catch (const gpu_index_direct_fill_invariant&) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
                    direct_fill_invariant_failed;
                return nullptr;
            }
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan minimizer index direct-fill invariant failed");
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error&) {
            throw;
        }
        if (!handle) return nullptr;

        if (cache_enabled) {
            insert_with_eviction_locked(key, handle);
        }
        return handle;
    }

    /// Insert a GPU-built resident index adopted from a device index build under `key`.
    ///
    /// The seed stage can then reuse it instead of uploading the CPU
    /// `minimizer_hashmap_fragment_index`.
    [[nodiscard]] std::expected<void, built_insert_error> insert_built(
        const gpu_minimizer_index_cache_key& key,
        std::shared_ptr<const gpu_resident_minimizer_index> handle)
    {
        if (!handle) {
            return std::unexpected{built_insert_error::null_handle};
        }
        std::lock_guard<std::mutex> guard(mu_);
        if (!key_gate_matches_binding_locked(key)) {
            return std::unexpected{
                built_insert_error::gate_identity_mismatch};
        }
        const auto key_gate_identity = gpu_index_gate_identity_from_key(key);
        if (!handle->gate_identity.is_well_formed()
            || !handle->gate_identity.is_exactly_equal_to(key_gate_identity)
            || (gate_binding_
                && !handle->gate_identity.is_exactly_equal_to(
                    gate_binding_->runtime_identity()))) {
            return std::unexpected{
                built_insert_error::gate_identity_mismatch};
        }
        if (!(handle->key == key)) {
            return std::unexpected{built_insert_error::resident_key_mismatch};
        }
        if (key.source_direct_sorted_dense && entries_.contains(key)) {
            return std::unexpected{built_insert_error::duplicate_direct_key};
        }
        const std::uint64_t handle_bytes = handle_resident_bytes(*handle);
        if (resident_limit_bytes_ != 0u
            && handle_bytes > resident_limit_bytes_) {
            return std::unexpected{built_insert_error::resident_limit};
        }
        insert_with_eviction_locked(key, handle);
        return {};
    }

    /// Required lookup for an operation-owned, already-published resident.
    /// This API has no CPU-index argument by construction, so a miss can never
    /// transcode or upload an empty placeholder. Runtime is read-only identity
    /// authority for the buffer owners and selected device UUID.
    ///
    /// @return The resident handle, or a typed key, miss, or resident-integrity error.
    [[nodiscard]] std::expected<
        std::shared_ptr<const gpu_resident_minimizer_index>,
        required_preinsert_lookup_error>
    find_required_preinserted(
        const bioseq::gpu::vulkan::runtime& runtime,
        const gpu_minimizer_index_cache_key& key)
    {
        std::lock_guard<std::mutex> guard(mu_);
        const bool device_uuid_bound = std::any_of(
            key.device_uuid.begin(), key.device_uuid.end(),
            [](std::uint8_t byte) noexcept { return byte != 0u; });
        if (!key.source_direct_sorted_dense
            || key.operation_identity == 0u
            || key.source_id != 0u
            || key.caller_identity != key.operation_identity
            || (key.alphabet_size != 4u && key.alphabet_size != 5u)
            || key.kmer_size != 15u || key.window_size != 10u
            || key.minimizer_size != 8u
            || key.minimizer_alignment != 8u
            || key.minimizer_hash_bits != 30u
            || key.minimizer_position_bits != 33u
            || !device_uuid_bound
            || key.device_uuid != runtime.features().device_uuid
            || key.representation != gpu_index_representation::source_direct_sorted_dense
            || key.sorted_dense_abi_revision
                != k_minimizer_sorted_dense_gpu_abi_revision
            || key.table_size == 0u || key.position_count == 0u
            || key.table_size > key.position_count
            || key.gate_enabled || !key_gate_matches_binding_locked(key)) {
            return std::unexpected{
                required_preinsert_lookup_error::invalid_key};
        }
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            return std::unexpected{required_preinsert_lookup_error::miss};
        }
        const auto& resident = found->second;
        const std::uint64_t required_control_bytes =
            bioseq::common::saturating_mul(
                key.table_size, sizeof(gpu_minimizer_slot));
        const std::uint64_t required_position_bytes =
            bioseq::common::saturating_mul(
                key.position_count, sizeof(std::uint64_t));
        const std::uint64_t actual_allocation_sum =
            bioseq::common::saturating_add(
                resident ? resident->slots.allocation_bytes : 0u,
                resident ? resident->positions.allocation_bytes : 0u);
        if (!resident || !(resident->key == key)
            || resident->representation
                != gpu_index_representation::source_direct_sorted_dense
            || resident->slot_mask != 0u
            || resident->bucket_count != key.table_size
            || resident->unique_key_count != key.table_size
            || resident->slot_capacity != key.table_size
            || resident->position_count != key.position_count
            || resident->target_len != key.target_len
            || resident->gate_identity.enabled
            || resident->accounting_authority
                == gpu_resident_accounting_authority::none
            || !resident->slots.is_valid() || !resident->positions.is_valid()
            || resident->slots.owner != &runtime
            || resident->positions.owner != &runtime
            || resident->slots.mapped == nullptr
            || resident->positions.mapped == nullptr
            || !resident->slots.is_host_visible_coherent()
            || !resident->positions.is_host_visible_coherent()
            || required_control_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || required_position_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || required_control_bytes > resident->slots.bytes
            || required_position_bytes > resident->positions.bytes
            || required_control_bytes > resident->slots.allocation_bytes
            || required_position_bytes > resident->positions.allocation_bytes
            || resident->source_stream_checksum == 0u
            || resident->source_stream_checksum
                != resident->mapped_stream_checksum
            || resident->complete_source_traversals != 1u
            || resident->complete_mapped_traversals != 1u
            || !gpu_exact_resident_authority_valid(
                resident->accounting_authority,
                resident->materialization,
                static_cast<bool>(resident->permit),
                resident->permit.bytes(),
                actual_allocation_sum)) {
            return std::unexpected{
                required_preinsert_lookup_error::resident_mismatch};
        }
        touch_lru_locked(key);
        return resident;
    }

    /**
     * Required lookup for a generated physical minimizer record.
     *
     * Unlike the legacy K15-only entry above, this overload authenticates the
     * cache key against the caller's already-selected generated record. It
     * still has no CPU-index or upload argument, so a miss cannot rebuild,
     * transcode, or silently select a host seed path.
     */
    template <class p_minimizer>
    [[nodiscard]] std::expected<
        std::shared_ptr<const gpu_resident_minimizer_index>,
        required_preinsert_lookup_error>
    find_required_preinserted_typed(
        const bioseq::gpu::vulkan::runtime& runtime,
        const gpu_minimizer_index_cache_key& key)
    {
        using layout = typename p_minimizer::layout_type;
        static_assert(std::is_standard_layout_v<p_minimizer>);
        static_assert(std::is_trivially_copyable_v<p_minimizer>);
        std::lock_guard<std::mutex> guard(mu_);
        const bool device_uuid_bound = std::any_of(
            key.device_uuid.begin(), key.device_uuid.end(),
            [](std::uint8_t byte) noexcept { return byte != 0u; });
        if (!gpu_source_direct_tuple_supported<p_minimizer>(
                key.alphabet_size, key.kmer_size, key.window_size) ||
            !key.source_direct_sorted_dense ||
            key.operation_identity == 0u || key.source_id != 0u ||
            key.caller_identity != key.operation_identity ||
            key.minimizer_size != sizeof(p_minimizer) ||
            key.minimizer_alignment != alignof(p_minimizer) ||
            key.minimizer_hash_bits != layout::k_hash_bits ||
            key.minimizer_position_bits != layout::k_position_bits ||
            layout::k_storage_bits != sizeof(p_minimizer) * 8u ||
            !device_uuid_bound || key.device_uuid != runtime.features().device_uuid ||
            key.representation != gpu_index_representation::source_direct_sorted_dense ||
            key.sorted_dense_abi_revision != k_minimizer_sorted_dense_gpu_abi_revision ||
            key.target_len == 0u || key.table_size == 0u || key.position_count == 0u ||
            key.table_size > key.position_count || key.gate_enabled ||
            !key_gate_matches_binding_locked(key)) {
            return std::unexpected{required_preinsert_lookup_error::invalid_key};
        }
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            return std::unexpected{required_preinsert_lookup_error::miss};
        }
        const auto& resident = found->second;
        const std::uint64_t required_control_bytes = bioseq::common::saturating_mul(
            key.table_size, sizeof(gpu_minimizer_slot));
        const std::uint64_t required_position_bytes = bioseq::common::saturating_mul(
            key.position_count, sizeof(std::uint64_t));
        const std::uint64_t actual_allocation_sum = bioseq::common::saturating_add(
            resident ? resident->slots.allocation_bytes : 0u,
            resident ? resident->positions.allocation_bytes : 0u);
        const bool source_traversals_valid =
            resident != nullptr &&
            (resident->complete_source_traversals == 1u ||
             resident->complete_source_traversals == 2u);
        if (!resident || !(resident->key == key) ||
            resident->representation != gpu_index_representation::source_direct_sorted_dense ||
            resident->slot_mask != 0u || resident->bucket_count != key.table_size ||
            resident->unique_key_count != key.table_size ||
            resident->slot_capacity != key.table_size ||
            resident->position_count != key.position_count ||
            resident->target_len != key.target_len || resident->gate_identity.enabled ||
            resident->accounting_authority == gpu_resident_accounting_authority::none ||
            !resident->slots.is_valid() || !resident->positions.is_valid() ||
            resident->slots.owner != &runtime || resident->positions.owner != &runtime ||
            resident->slots.mapped == nullptr || resident->positions.mapped == nullptr ||
            !resident->slots.is_host_visible_coherent() ||
            !resident->positions.is_host_visible_coherent() ||
            required_control_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            required_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            required_control_bytes > resident->slots.bytes ||
            required_position_bytes > resident->positions.bytes ||
            required_control_bytes > resident->slots.allocation_bytes ||
            required_position_bytes > resident->positions.allocation_bytes ||
            resident->source_stream_checksum == 0u ||
            resident->source_stream_checksum != resident->mapped_stream_checksum ||
            !source_traversals_valid || resident->complete_mapped_traversals != 1u ||
            !gpu_exact_resident_authority_valid(resident->accounting_authority,
                                                resident->materialization,
                                                static_cast<bool>(resident->permit),
                                                resident->permit.bytes(),
                                                actual_allocation_sum)) {
            return std::unexpected{required_preinsert_lookup_error::resident_mismatch};
        }
        touch_lru_locked(key);
        return resident;
    }

    /// Materialize a prepared host upload and optionally retain it in the cache.
    ///
    /// Returns null when memory admission fails or mapped device storage is unavailable.
    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> insert_upload(
        bioseq::gpu::vulkan::runtime& runtime,
        gpu_minimizer_index_upload&& upload,
        const gpu_minimizer_index_cache_key& key,
        bool cache_enabled)
    {
        std::lock_guard<std::mutex> guard(mu_);
        validate_key_gate_locked(key);
        if (cache_enabled) {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                touch_lru_locked(key);
                return found->second;
            }
        }

        auto handle = upload_prepared_locked(runtime, upload, key);
        if (!handle) return nullptr;
        if (cache_enabled) {
            insert_with_eviction_locked(key, handle);
        }
        return handle;
    }

    /// Remove one key from cache accounting without invalidating shared resident handles.
    void erase(const gpu_minimizer_index_cache_key& key) noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        auto found = entries_.find(key);
        if (found == entries_.end()) return;
        resident_bytes_ -= handle_resident_bytes(*found->second);
        entries_.erase(found);
        auto pos = std::find(lru_.begin(), lru_.end(), key);
        if (pos != lru_.end()) lru_.erase(pos);
    }

    /// Remove every cached entry and release the cache-wide gate binding.
    void clear() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        entries_.clear();
        lru_.clear();
        resident_bytes_ = 0;
        gate_binding_.reset();
    }

    /// Return the allocation bytes attributed to currently cached residents.
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return resident_bytes_;
    }

private:
    /// Check a key against the canonical gate binding while the cache mutex is held.
    [[nodiscard]] bool key_gate_matches_binding_locked(
        const gpu_minimizer_index_cache_key& key) const noexcept
    {
        if (gate_binding_) {
            return gate_binding_->is_well_formed()
                && gate_binding_->metadata_matches(key);
        }
        // A gate-enabled key without an exact canonical binding would make a
        // digest authoritative.  Fail it closed.  The ordinary ungated cache
        // remains usable without an explicit bind.
        return !key.gate_enabled && key.gate_word_count == 0u
            && key.gate_mask == 0u && key.gate_digest_lo == 0u
            && key.gate_digest_hi == 0u && !key.gate_canonical_words;
    }

    /// Throw when a key does not match the cache's canonical gate binding.
    void validate_key_gate_locked(
        const gpu_minimizer_index_cache_key& key) const
    {
        if (!key_gate_matches_binding_locked(key)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan minimizer-index cache gate identity mismatch");
        }
    }

    /// Sum the resident slot and position allocation sizes with saturation.
    [[nodiscard]] static std::uint64_t handle_resident_bytes(
        const gpu_resident_minimizer_index& handle) noexcept
    {
        return bioseq::common::saturating_add(
            handle.slots.allocation_bytes,
            handle.positions.allocation_bytes);
    }

    /// Move an existing key to the most-recently-used end of the LRU list.
    void touch_lru_locked(const gpu_minimizer_index_cache_key& key) {
        auto pos = std::find(lru_.begin(), lru_.end(), key);
        if (pos != lru_.end()) {
            lru_.splice(lru_.begin(), lru_, pos);
        }
    }

    /// Replace or insert a resident and evict least-recently-used entries to budget.
    void insert_with_eviction_locked(
        const gpu_minimizer_index_cache_key& key,
        const std::shared_ptr<const gpu_resident_minimizer_index>& handle)
    {
        const std::uint64_t handle_bytes = handle_resident_bytes(*handle);
        // `insert_built()` may publish a replacement under a key that is
        // already resident. Remove the previous accounting and every stale
        // LRU occurrence before considering the new allocation. Otherwise a
        // replacement double-counts bytes and can later evict the wrong handle.
        if (auto existing = entries_.find(key); existing != entries_.end()) {
            const std::uint64_t existing_bytes =
                handle_resident_bytes(*existing->second);
            resident_bytes_ = existing_bytes <= resident_bytes_
                ? resident_bytes_ - existing_bytes
                : 0u;
            entries_.erase(existing);
            lru_.remove(key);
        }
        // A handle larger than the whole budget is used uncached after it is
        // returned to the caller. Do not pollute the cache with it.
        if (resident_limit_bytes_ != 0 && handle_bytes > resident_limit_bytes_) {
            return;
        }
        // Test whether adding the new allocation would exceed the configured resident budget.
        const auto exceeds_resident_limit = [this, &handle_bytes]() noexcept {
            return resident_limit_bytes_ != 0
                && (resident_bytes_ > resident_limit_bytes_
                    || handle_bytes
                        > resident_limit_bytes_ - resident_bytes_);
        };
        while (!lru_.empty() && exceeds_resident_limit()) {
            const gpu_minimizer_index_cache_key victim = lru_.back();
            lru_.pop_back();
            auto found = entries_.find(victim);
            if (found != entries_.end()) {
                resident_bytes_ -= handle_resident_bytes(*found->second);
                entries_.erase(found);
            }
        }
        entries_[key] = handle;
        lru_.push_front(key);
        resident_bytes_ = bioseq::common::saturating_add(
            resident_bytes_, handle_bytes);
    }

    /// Direct-fill a CPU fragment index into admitted mapped Vulkan buffers.
    ///
    /// The cache mutex is held by the caller. Expected resource limits return null when a decline
    /// output is present, while device ABI and fill invariant failures otherwise throw.
    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> upload_locked_index(
        bioseq::gpu::vulkan::runtime& runtime,
        const minimizer_hashmap_fragment_index& idx,
        const gpu_minimizer_index_cache_key& key,
        gpu_index_upload_decline* decline_out)
    {
        using bioseq::parallelism::memory_admission_controller;
        if (idx.positions.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
            throw gpu_index_too_large{};
        }
        const std::uint64_t upload_peak = gpu_index_upload_peak_bytes(idx);
        const std::uint64_t unique_key_count = idx.table_size();
        const gpu_index_materialization_plan open_plan =
            plan_gpu_index_materialization(
                unique_key_count, idx.positions.size(),
                gpu_index_representation::open_addressed_70);
        std::optional<gpu_index_materialization_plan> dense_plan;
        if (unique_key_count != 0u
            && unique_key_count
                <= (std::numeric_limits<std::uint32_t>::max)()) {
            dense_plan = plan_gpu_index_materialization(
                unique_key_count, idx.positions.size(),
                gpu_index_representation::source_direct_sorted_dense);
        }
        const std::uint64_t max_storage_buffer_range =
            runtime.features().max_storage_buffer_range;
        // Check both planned device ranges against the selected device limit.
        const auto ranges_fit = [max_storage_buffer_range](
                const gpu_index_materialization_plan& candidate) noexcept {
            return gpu_index_storage_ranges_fit(
                candidate.slot_buffer_bytes,
                candidate.position_buffer_bytes,
                max_storage_buffer_range);
        };
        gpu_index_materialization_plan plan = open_plan;
        if (!ranges_fit(open_plan) && dense_plan) {
            plan = *dense_plan;
        }
        // Copy the currently selected materialization plan into optional decline diagnostics.
        const auto publish_plan = [&decline_out, &upload_peak, &max_storage_buffer_range](
                                      const gpu_index_materialization_plan& selected) {
            if (decline_out == nullptr)
                return;
            decline_out->resident_bytes = selected.resident_bytes;
            decline_out->upload_peak_bytes = upload_peak;
            decline_out->slot_bytes = selected.slot_bytes;
            decline_out->position_bytes = selected.position_bytes;
            decline_out->unique_key_count = selected.unique_key_count;
            decline_out->slot_capacity = selected.slot_capacity;
            decline_out->load_ppm = selected.load_ppm;
            decline_out->representation = selected.representation;
            decline_out->staging_bytes = 0u;
            decline_out->max_storage_buffer_range =
                max_storage_buffer_range;
        };
        publish_plan(plan);

        if (!ranges_fit(plan)) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
                    storage_buffer_range_exceeded;
            }
            return nullptr;
        }

        auto& admission_controller = memory_admission_controller::instance();
        bioseq::parallelism::memory_admission_failure admission_failure{};
        auto resident_permit =
            admission_controller.try_reserve(
                plan.resident_bytes, &admission_failure);
        if (!resident_permit
            && plan.representation
                == gpu_index_representation::open_addressed_70
            && dense_plan
            && dense_plan->resident_bytes < plan.resident_bytes
            && ranges_fit(*dense_plan)) {
            const auto open_failure = admission_failure;
            plan = *dense_plan;
            publish_plan(plan);
            admission_failure = {};
            bioseq_log<logging_level::info>(
                "[gpu_index_representation_adaptation] from={} to={} "
                "open_resident_bytes={} dense_resident_bytes={} "
                "open_admission_reason={} open_budget_ceiling_bytes={} "
                "open_allocatable_after_reservations_bytes={}",
                gpu_index_representation_name(
                    gpu_index_representation::open_addressed_70),
                gpu_index_representation_name(
                    gpu_index_representation::source_direct_sorted_dense),
                open_plan.resident_bytes,
                plan.resident_bytes,
                bioseq::parallelism::memory_admission_failure_reason_name(
                    open_failure.reason),
                open_failure.budget_ceiling_bytes,
                open_failure.allocatable_after_reservations_bytes);
            resident_permit = admission_controller.try_reserve(
                plan.resident_bytes, &admission_failure);
        }
        if (!resident_permit) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
                    resident_memory_admission_denied;
                decline_out->admission = admission_failure;
            }
            return nullptr;
        }

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<gpu_resident_minimizer_index>();
        gpu_index_direct_fill_stats fill_stats{};
        bool allocation_growth_denied = false;
        std::uint64_t denied_actual_allocation_bytes = 0u;
        bool non_host_visible = false;
        bioseq::parallelism::memory_admission_failure growth_failure{};
        const auto materialization = admission_controller.materialize(
            *resident_permit,
            [&resident,
             &runtime,
             &plan,
             &resident_permit,
             &growth_failure,
             &allocation_growth_denied,
             &denied_actual_allocation_bytes,
             &non_host_visible,
             &idx,
             &fill_stats]() -> bioseq::parallelism::memory_materialization_snapshot {
                // Prefix lookup repeatedly reads this resident on the CPU.
                // Keep its Vulkan-visible payload in cached host memory.
                resident->slots = runtime.create_host_buffer(
                    plan.slot_buffer_bytes, usage);
                resident->positions = runtime.create_host_buffer(
                    plan.position_buffer_bytes, usage);
                const std::uint64_t actual_allocation_bytes =
                    bioseq::common::saturating_add(
                        resident->slots.allocation_bytes,
                        resident->positions.allocation_bytes);
                if (!resident_permit->grow_to(
                        actual_allocation_bytes, &growth_failure)) {
                    allocation_growth_denied = true;
                    denied_actual_allocation_bytes = actual_allocation_bytes;
                    resident->positions.reset();
                    resident->slots.reset();
                    return {};
                }
                if (resident->slots.mapped == nullptr
                    || resident->positions.mapped == nullptr) {
                    non_host_visible = true;
                    return {
                        .actual_allocation_bytes = actual_allocation_bytes,
                        .is_host_visible = false,
                        .is_host_coherent = false,
                        .is_process_footprint_reflected = false,
                    };
                }

                const auto mapped_slots = std::span<gpu_minimizer_slot>{
                    static_cast<gpu_minimizer_slot*>(resident->slots.mapped),
                    static_cast<std::size_t>(plan.slot_capacity)};
                const auto mapped_positions = std::span<std::uint64_t>{
                    static_cast<std::uint64_t*>(resident->positions.mapped),
                    idx.positions.size()};
                fill_stats = plan.representation
                        == gpu_index_representation::source_direct_sorted_dense
                    ? direct_fill_gpu_minimizer_index_sorted_dense(
                        idx, mapped_slots, mapped_positions)
                    : direct_fill_gpu_minimizer_index(
                        idx, mapped_slots, mapped_positions);
                if (resident->slots.allocation_bytes > plan.slot_bytes) {
                    std::memset(
                        static_cast<std::byte*>(resident->slots.mapped)
                            + plan.slot_bytes,
                        0,
                        static_cast<std::size_t>(
                            resident->slots.allocation_bytes - plan.slot_bytes));
                }
                if (resident->positions.allocation_bytes > plan.position_bytes) {
                    std::memset(
                        static_cast<std::byte*>(resident->positions.mapped)
                            + plan.position_bytes,
                        0,
                        static_cast<std::size_t>(resident->positions.allocation_bytes
                            - plan.position_bytes));
                }
                const bool host_visible =
                    (resident->slots.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u
                    && (resident->positions.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u;
                const bool host_coherent =
                    (resident->slots.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u
                    && (resident->positions.memory_properties
                        & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
                return {
                    .actual_allocation_bytes = actual_allocation_bytes,
                    .fully_touched_bytes = actual_allocation_bytes,
                    .is_host_visible = host_visible,
                    .is_host_coherent = host_coherent,
                    .is_process_footprint_reflected =
                        resident->slots.process_footprint_reflected
                        && resident->positions.process_footprint_reflected,
                };
            });
        if (allocation_growth_denied) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
                    resident_memory_admission_denied;
                decline_out->resident_bytes =
                    denied_actual_allocation_bytes;
                decline_out->actual_allocation_bytes =
                    denied_actual_allocation_bytes;
                decline_out->admission = growth_failure;
            }
            return nullptr;
        }
        if (non_host_visible) {
            if (decline_out != nullptr) {
                decline_out->reason = gpu_index_upload_decline_reason::
                    non_host_visible_device_memory;
            }
            return nullptr;
        }
        resident->permit = std::move(*resident_permit);
        resident->key = key;
        resident->gate_identity = gpu_index_gate_identity_from_key(key);
        resident->target_len = idx.target_len;
        resident->slot_mask = plan.representation
                == gpu_index_representation::open_addressed_70
            ? static_cast<std::uint32_t>(plan.slot_capacity - 1u)
            : 0u;
        resident->position_count = static_cast<std::uint32_t>(idx.positions.size());
        resident->bucket_count = plan.representation
                == gpu_index_representation::source_direct_sorted_dense
            ? static_cast<std::uint32_t>(plan.slot_capacity)
            : 0u;
        resident->unique_key_count = fill_stats.unique_key_count;
        resident->slot_capacity = fill_stats.slot_capacity;
        resident->load_ppm = fill_stats.load_ppm;
        resident->max_probe = fill_stats.max_probe;
        resident->representation = plan.representation;
        resident->staging_bytes = 0u;
        resident->materialization = materialization;
        if (decline_out != nullptr) {
            decline_out->max_probe = fill_stats.max_probe;
            decline_out->actual_allocation_bytes =
                materialization.allocation_snapshot.actual_allocation_bytes;
            decline_out->fully_touched_bytes =
                materialization.allocation_snapshot.fully_touched_bytes;
            decline_out->reflected_footprint_delta_bytes =
                materialization.reflected_delta_bytes;
            decline_out->materialization_permit_released =
                materialization.is_permit_released;
            decline_out->process_footprint_reflected =
                materialization.allocation_snapshot
                    .is_process_footprint_reflected;
        }
        bioseq_log<logging_level::info>(
            "[gpu_index_materialization_success] representation={} unique_keys={} "
            "slot_capacity={} load_ppm={} max_probe={} logical_bytes={} "
            "actual_allocation_bytes={} fully_touched_bytes={} "
            "staging_bytes=0 permit_released={} footprint_reflected={} "
            "reflected_delta_bytes={}",
            gpu_index_representation_name(plan.representation),
            fill_stats.unique_key_count,
            fill_stats.slot_capacity,
            fill_stats.load_ppm,
            fill_stats.max_probe,
            plan.resident_bytes,
            materialization.allocation_snapshot.actual_allocation_bytes,
            materialization.allocation_snapshot.fully_touched_bytes,
            materialization.is_permit_released ? 1 : 0,
            materialization.allocation_snapshot
                .is_process_footprint_reflected ? 1 : 0,
            materialization.reflected_delta_bytes);
        return resident;
    }

    /// Copy a prepared host image into admitted mapped Vulkan buffers.
    ///
    /// The cache mutex is held by the caller. The returned handle owns both buffers and its
    /// admission permit, or is null if reservation or host mapping fails.
    [[nodiscard]] std::shared_ptr<const gpu_resident_minimizer_index> upload_prepared_locked(
        bioseq::gpu::vulkan::runtime& runtime,
        const gpu_minimizer_index_upload& upload,
        const gpu_minimizer_index_cache_key& key)
    {
        using bioseq::parallelism::memory_admission_controller;
        const std::uint64_t slot_bytes =
            static_cast<std::uint64_t>(upload.slots.size()) * sizeof(gpu_minimizer_slot);
        const std::uint64_t pos_bytes =
            static_cast<std::uint64_t>(upload.positions.size()) * sizeof(std::uint64_t);
        const std::uint64_t resident_bytes =
            bioseq::common::saturating_add(slot_bytes, pos_bytes);
        auto resident_permit =
            memory_admission_controller::instance().try_reserve(resident_bytes);
        if (!resident_permit) return nullptr;

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<gpu_resident_minimizer_index>();
        resident->slots = runtime.create_host_buffer(std::max<std::uint64_t>(slot_bytes, 16u), usage);
        resident->positions = runtime.create_host_buffer(std::max<std::uint64_t>(pos_bytes, 8u), usage);
        if (resident->slots.mapped == nullptr || resident->positions.mapped == nullptr) {
            return nullptr;
        }
        if (slot_bytes > 0) {
            std::memcpy(resident->slots.mapped, upload.slots.data(),
                        static_cast<std::size_t>(slot_bytes));
        }
        if (pos_bytes > 0) {
            std::memcpy(resident->positions.mapped, upload.positions.data(),
                        static_cast<std::size_t>(pos_bytes));
        }
        resident->permit = std::move(*resident_permit);
        resident->key = key;
        resident->gate_identity = gpu_index_gate_identity_from_key(key);
        resident->target_len = upload.target_len;
        resident->slot_mask = upload.slot_mask;
        resident->position_count = static_cast<std::uint32_t>(upload.positions.size());
        resident->bucket_count = 0u;
        return resident;
    }

    std::uint64_t resident_bytes_ = 0;
    std::uint64_t resident_limit_bytes_ = 0;
    std::optional<gate_identity> gate_binding_{};
    mutable std::mutex mu_;
    std::list<gpu_minimizer_index_cache_key> lru_;
    std::unordered_map<gpu_minimizer_index_cache_key,
        std::shared_ptr<const gpu_resident_minimizer_index>,
        gpu_minimizer_index_cache_key_hash> entries_;
};

} // namespace bioseq::indexing
