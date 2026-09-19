/**
 * @file typed_support_parts/transient_storage.cppm
 * @brief Defines transient fragment and minimizer-sequence storage owners.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}
#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.transient_storage;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.common.heap_allocation;
import bioseq.common.logging;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.pipeline.pipe;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.persist_target;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Flush one exact-array failure record without replacing its owning failure. */
inline void emit_transient_exact_array_failure(
    const char* site,
    std::uint64_t count,
    std::uint64_t element_bytes,
    std::uint64_t requested_bytes,
    std::uint64_t allocator_request_bytes,
    std::uint64_t allocation_requirement_bytes,
    std::uint64_t permit_bytes,
    std::uint64_t admitted_bytes,
    std::uint64_t actual_bytes,
    std::uint64_t reported_bytes,
    bool permit_released,
    const bioseq::parallelism::memory_admission_failure* failure = nullptr) noexcept {
    const auto reason = failure != nullptr && failure->has_failure()
                            ? bioseq::parallelism::memory_admission_failure_reason_name(
                                  failure->reason)
                            : std::string_view{"none"};
    try {
        bioseq_log_progress<logging_level::warning, logging_target::file>(
            "[bioseq.transient_exact_array.failure] site={} count={} element_bytes={} "
            "requested_bytes={} allocator_request_bytes={} allocation_requirement_bytes={} "
            "permit_bytes={} admitted_bytes={} actual_bytes={} reported_bytes={} "
            "permit_released={} admission_reason={} admission_requested_bytes={} "
            "reserved_bytes={} budget_ceiling_bytes={} allocatable_after_reservations_bytes={} "
            "process_footprint_bytes={} system_available_bytes={} system_total_bytes={} "
            "process_soft_cap_bytes={}",
            site,
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(element_bytes),
            static_cast<unsigned long long>(requested_bytes),
            static_cast<unsigned long long>(allocator_request_bytes),
            static_cast<unsigned long long>(allocation_requirement_bytes),
            static_cast<unsigned long long>(permit_bytes),
            static_cast<unsigned long long>(admitted_bytes),
            static_cast<unsigned long long>(actual_bytes),
            static_cast<unsigned long long>(reported_bytes),
            permit_released,
            reason,
            static_cast<unsigned long long>(failure != nullptr ? failure->requested_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr ? failure->reserved_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->budget_ceiling_bytes
                                                : 0u),
            static_cast<unsigned long long>(
                failure != nullptr ? failure->allocatable_after_reservations_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->process_footprint_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->system_available_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->system_total_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->process_soft_cap_bytes
                                                : 0u));
    } catch (...) {
        // Diagnostics must not replace the existing false return or allocation exception.
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
#endif

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Grow one vector while an adjacent permit covers retained and replacement capacities. */
template <class p_value>
[[nodiscard]] bool ensure_transient_vector_capacity(
    bioseq::parallelism::memory_permit& permit,
    std::vector<p_value>& values,
    std::size_t required,
    std::uint64_t other_retained_bytes = 0u) {
    const std::uint64_t old_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(p_value)));
    const std::uint64_t retained =
        bioseq::common::saturating_add(other_retained_bytes, old_bytes);
    if (old_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        retained == (std::numeric_limits<std::uint64_t>::max)() ||
        (retained != 0u && (!permit || permit.bytes() < retained))) {
        return false;
    }
    if (values.capacity() >= required) {
        return true;
    }
    if (required > values.max_size()) {
        return false;
    }
    std::size_t requested = values.capacity() == 0u
                                ? std::min<std::size_t>(8u, values.max_size())
                                : values.capacity();
    while (requested < required) {
        if (requested > values.max_size() / 2u) {
            requested = required;
            break;
        }
        requested *= 2u;
    }
    requested = std::max(requested, required);
    const std::uint64_t requested_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(requested),
        static_cast<std::uint64_t>(sizeof(p_value)));
    const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
        requested_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t peak = bioseq::common::saturating_add(retained, allocation_headroom);
    if (requested_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        allocation_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
        peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (permit) {
        if (!permit.grow_to(peak)) {
            return false;
        }
    } else {
        auto acquired =
            bioseq::parallelism::memory_admission_controller::instance().try_reserve(peak);
        if (!acquired) {
            return false;
        }
        permit = std::move(*acquired);
    }
    try {
        values.reserve(requested);
    } catch (...) {
        permit.shrink_to(retained);
        throw;
    }
    const std::uint64_t measured = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(p_value)));
    const std::uint64_t reconciled =
        bioseq::common::saturating_add(other_retained_bytes, measured);
    if (measured == (std::numeric_limits<std::uint64_t>::max)() ||
        measured > allocation_headroom ||
        reconciled == (std::numeric_limits<std::uint64_t>::max)() ||
        reconciled > permit.bytes()) {
        return false;
    }
    permit.shrink_to(reconciled);
    return true;
}

/** Exact native array owner used after a completed spill supplies its real count. */
template <class p_value> struct transient_materialized_array {
    /** Stateless deleter for the audited native-heap allocation. */
    struct allocation_deleter {
        void operator()(p_value* allocation) const noexcept {
            std::free(allocation);
        }
    };

    // Permit precedes storage so an active permit outlives deallocation.
    bioseq::parallelism::memory_permit permit;
    std::unique_ptr<p_value, allocation_deleter> values{};
    std::uint64_t allocation_bytes = 0u;
    std::size_t size = 0u;
    std::size_t capacity = 0u;
    bool permit_released_to_footprint = false;

    /** Return whether capacity is covered by exactly one authority. */
    [[nodiscard]] bool valid() const noexcept {
        if (allocation_bytes == 0u) {
            return values == nullptr && size == 0u && capacity == 0u && !permit &&
                   !permit_released_to_footprint;
        }
        return values != nullptr && size <= capacity &&
               permit_released_to_footprint != static_cast<bool>(permit);
    }
};

/** Admit, allocate, and touch one exact-count array after the logical count is known. */
template <class p_value>
[[nodiscard]] bool prepare_transient_array_exact(
    transient_materialized_array<p_value>& owner,
    std::size_t count) {
    static_assert(std::is_trivially_copyable_v<p_value>);
    static_assert(std::is_trivially_destructible_v<p_value>);
    if (owner.permit || owner.values != nullptr || owner.allocation_bytes != 0u ||
        owner.size != 0u || owner.capacity != 0u || owner.permit_released_to_footprint) {
        return false;
    }
    if (count == 0u) {
        return true;
    }
    const std::uint64_t requested_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(count), static_cast<std::uint64_t>(sizeof(p_value)));
    if (requested_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        requested_bytes >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    bioseq::common::heap_allocation_detail::heap_allocation_plan allocation_plan{};
    try {
        allocation_plan = bioseq::common::heap_allocation_detail::plan_heap_allocation(
            static_cast<std::size_t>(requested_bytes));
    } catch (const std::exception&) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        emit_transient_exact_array_failure("allocation_plan_invalid",
                                           static_cast<std::uint64_t>(count),
                                           sizeof(p_value),
                                           requested_bytes,
                                           0u,
                                           0u,
                                           0u,
                                           0u,
                                           0u,
                                           0u,
                                           false);
#endif
        return false;
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = admission.try_reserve(
        static_cast<std::uint64_t>(allocation_plan.allocation_requirement_bytes),
        std::addressof(admission_failure));
#else
    auto permit = admission.try_reserve(
        static_cast<std::uint64_t>(allocation_plan.allocation_requirement_bytes));
#endif
    if (!permit) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        emit_transient_exact_array_failure(
            "reservation_denied",
            static_cast<std::uint64_t>(count),
            sizeof(p_value),
            requested_bytes,
            static_cast<std::uint64_t>(allocation_plan.allocator_request_bytes),
            static_cast<std::uint64_t>(allocation_plan.allocation_requirement_bytes),
            0u,
            0u,
            0u,
            0u,
            false,
            std::addressof(admission_failure));
#endif
        return false;
    }
    owner.permit = std::move(*permit);
    const auto materialization = admission.materialize(
        owner.permit,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        [&owner, allocation_plan, count, requested_bytes]() {
#else
        [&owner, allocation_plan]() {
#endif
            std::size_t actual = 0u;
            void* allocation = bioseq::common::heap_allocation_detail::allocate_and_touch(
                allocation_plan, actual);
            if (allocation == nullptr || actual == 0u || actual > owner.permit.bytes()) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                emit_transient_exact_array_failure(
                    "physical_allocation_or_extent_invalid",
                    static_cast<std::uint64_t>(count),
                    sizeof(p_value),
                    requested_bytes,
                    static_cast<std::uint64_t>(allocation_plan.allocator_request_bytes),
                    static_cast<std::uint64_t>(allocation_plan.allocation_requirement_bytes),
                    owner.permit.bytes(),
                    owner.permit.bytes(),
                    static_cast<std::uint64_t>(actual),
                    static_cast<std::uint64_t>(actual),
                    false);
#endif
                throw std::bad_alloc{};
            }
            owner.values.reset(static_cast<p_value*>(allocation));
            owner.permit.shrink_to(static_cast<std::uint64_t>(actual));
            return bioseq::parallelism::memory_materialization_snapshot{
                .actual_allocation_bytes = static_cast<std::uint64_t>(actual),
                .fully_touched_bytes = static_cast<std::uint64_t>(actual),
                .is_host_visible = true,
                .is_host_coherent = true,
                .is_process_footprint_reflected = true,
            };
        });
    owner.allocation_bytes = materialization.allocation_snapshot.actual_allocation_bytes;
    owner.capacity = count;
    owner.permit_released_to_footprint = materialization.is_permit_released;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    const std::uint64_t reported_bytes =
        bioseq::common::heap_allocation_detail::allocator_reported_bytes(owner.values.get());
    const bool reconciled = materialization.admitted_bytes == owner.allocation_bytes &&
                            owner.allocation_bytes >= requested_bytes &&
                            reported_bytes == owner.allocation_bytes && owner.valid();
    if (!reconciled) {
        emit_transient_exact_array_failure(
            "materialization_reconcile_failed",
            static_cast<std::uint64_t>(count),
            sizeof(p_value),
            requested_bytes,
            static_cast<std::uint64_t>(allocation_plan.allocator_request_bytes),
            static_cast<std::uint64_t>(allocation_plan.allocation_requirement_bytes),
            owner.permit.bytes(),
            materialization.admitted_bytes,
            owner.allocation_bytes,
            reported_bytes,
            materialization.is_permit_released);
    }
    return reconciled;
#else
    return materialization.admitted_bytes == owner.allocation_bytes &&
           owner.allocation_bytes >= requested_bytes &&
           bioseq::common::heap_allocation_detail::allocator_reported_bytes(owner.values.get()) ==
               owner.allocation_bytes &&
           owner.valid();
#endif
}

/** In-memory byte payload for one opportunistic fragment-index chunk. */
struct opportunistic_index_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::vector<char> bytes{};
};

/** Transient in-memory chunked storage for opportunistic minimizer fragment indexes. */
struct opportunistic_fragment_index_storage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::vector<opportunistic_index_chunk> chunks{};

    /** Raises a PostgreSQL data-corruption error and does not return. */
    [[noreturn]] void index_error(std::string_view msg) const {
        bioseq::postgres::support::invoke_pg_as_cpp([msg] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq opportunistic index: %.*s",
                            static_cast<int>(msg.size()),
                            msg.data())));
        });
        pg_unreachable();
    }

    /** Validates encoded minimizer blob header, layout revision, and total byte size. */
    [[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header
    validate_minimizer_blob_or_error(
        const char* blob_data,
        std::size_t bytes,
        std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
        using bucket_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type)) {
            index_error("minimizer blob truncated header");
        }
        header_type header{};
        std::memcpy(&header, blob_data, sizeof(header_type));
        if (header.magic != bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_magic ||
            header.layout_revision !=
                bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_layout_revision) {
            index_error("minimizer blob magic/layout");
        }
        const std::size_t expected_bytes = sizeof(header_type) +
                                           sizeof(bucket_type) * header.bucket_count +
                                           sizeof(std::uint64_t) * header.total_positions;
        if (bytes != expected_bytes) {
            index_error("minimizer blob size mismatch");
        }
        if (expected_bucket_count && header.bucket_count != *expected_bucket_count) {
            index_error("minimizer blob bucket_count mismatch");
        }
        return header;
    }

    /** Rejects directory blobs shorter than one int64. */
    void validate_minimizer_directory_or_error(const char*, std::size_t bytes) const {
        if (bytes < sizeof(std::int64_t)) {
            index_error("directory truncated");
        }
    }

    /** Rejects chunk metadata when stored byte size is below @p min_bytes. */
    void validate_minimizer_chunk_metadata_or_error(bioseq::indexing::index_sub_key_t sub_key,
                                                    std::size_t chunk_no,
                                                    std::size_t min_bytes,
                                                    std::size_t) const {
        if (chunk_byte_size(sub_key, chunk_no) < min_bytes) {
            index_error("chunk metadata below minimum size");
        }
    }

    /** Returns how many chunks belong to @p sub_key. */
    [[nodiscard]] std::size_t number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t chunk_count = 0;
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++chunk_count;
            }
        }
        return chunk_count;
    }

    /** Counts distinct sub-keys in chunk append order. */
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t sub_key_count = 0;
        bioseq::indexing::index_sub_key_t last_sub_key =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have_sub_key = false;
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (!have_sub_key || chunk.sub_key != last_sub_key) {
                ++sub_key_count;
                last_sub_key = chunk.sub_key;
                have_sub_key = true;
            }
        }
        return sub_key_count;
    }

    /** Returns stored byte length for a chunk, or zero when missing. */
    [[nodiscard]] std::size_t chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key,
                                              std::size_t chunk_no) const {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk.bytes.size();
            }
        }
        return 0;
    }

    /** Returns a typed view into chunk bytes or reports a missing chunk. */
    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                            std::size_t chunk_no) {
        for (opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return reinterpret_cast<const p_value*>(chunk.bytes.data());
            }
        }
        index_error("chunk missing");
    }

    /** Plans fixed-size chunks that cover @p bytes_count for @p sub_key. */
    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t chunk_count =
            bytes_count == 0 ? 1
                             : (bytes_count + bioseq::indexing::k_max_index_chunk_size_bytes - 1) /
                                   bioseq::indexing::k_max_index_chunk_size_bytes;
        std::vector<std::size_t> chunk_plan(chunk_count);
        std::size_t remaining_bytes = bytes_count;
        for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const std::size_t chunk_bytes =
                std::min(remaining_bytes, bioseq::indexing::k_max_index_chunk_size_bytes);
            chunk_plan[chunk_index] = chunk_bytes;
            remaining_bytes -= chunk_bytes;
        }
        plans[sub_key] = std::move(chunk_plan);
    }

    /** Replaces the per-chunk byte plan for @p sub_key. */
    void resize_chunks(bioseq::indexing::index_sub_key_t sub_key,
                       std::span<const std::size_t> per_chunk_bytes) {
        plans[sub_key] = std::vector<std::size_t>(per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    /** No-op prefetch hook required by the storage concept. */
    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    /** Materializes planned chunks and fills each via @p writer. */
    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        auto plan_iterator = plans.find(sub_key);
        if (plan_iterator == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& chunk_plan = plan_iterator->second;
        for (std::size_t chunk_no = 0; chunk_no < chunk_plan.size();) {
            const std::size_t payload_bytes = chunk_plan[chunk_no];
            if (payload_bytes % sizeof(p_value) != 0) {
                index_error("chunk payload is not divisible by the value type size");
            }
            opportunistic_index_chunk chunk{};
            chunk.sub_key = sub_key;
            chunk.chunk_no = static_cast<std::uint32_t>(chunk_no);
            chunk.bytes.assign(payload_bytes, char{0});
            const std::size_t next = writer(chunk_no,
                                            reinterpret_cast<p_value*>(chunk.bytes.data()),
                                            payload_bytes / sizeof(p_value));
            if (next < chunk_no) {
                index_error("writer moved backwards");
            }
            chunks.push_back(std::move(chunk));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(plan_iterator);
    }

    /** Invokes @p callback for chunks whose sub-key lies in [@p lower_bound, @p upper_bound). */
    template <class p_function>
    void read_chunks_in_range(bioseq::indexing::index_sub_key_t lower_bound,
                              bioseq::indexing::index_sub_key_t upper_bound,
                              p_function&& callback) {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key >= lower_bound && chunk.sub_key < upper_bound) {
                callback(chunk.sub_key, chunk.bytes.data(), chunk.bytes.size());
            }
        }
    }

    /** Calls @p callback once per distinct sub-key in chunk order. */
    void for_each_sub_key(auto&& callback) const {
        bioseq::indexing::index_sub_key_t last_sub_key =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have_sub_key = false;
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (!have_sub_key || chunk.sub_key != last_sub_key) {
                callback(chunk.sub_key);
                last_sub_key = chunk.sub_key;
                have_sub_key = true;
            }
        }
    }

    /** Drops all chunk payloads and pending resize plans. */
    void clear_entity() {
        plans.clear();
        chunks.clear();
    }

    /** Clears transient storage and ignores the name argument. */
    void clear(std::string_view) {
        clear_entity();
    }

    /** Removes chunks for sub-keys in the inclusive [@p first_sub_key, @p last_sub_key] range. */
    void discard_sub_keys(bioseq::indexing::index_sub_key_t first_sub_key,
                          bioseq::indexing::index_sub_key_t last_sub_key) {
        chunks.erase(std::remove_if(
                         chunks.begin(),
                         chunks.end(),
                         [first_sub_key, last_sub_key](const opportunistic_index_chunk& chunk) {
                             return chunk.sub_key >= first_sub_key && chunk.sub_key <= last_sub_key;
                         }),
                     chunks.end());
    }

    /** Reports an index error because this operation is unsupported. */
    [[nodiscard]] std::size_t
    spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    /** Reports an index error because this operation is unsupported. */
    [[nodiscard]] std::size_t
    spill_sorted_run_owned(std::vector<std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    /** Releases @p permit and reports an index error because this operation is unsupported. */
    [[nodiscard]] std::size_t
    spill_sorted_run_owned_admitted(std::vector<std::pair<std::uint64_t, std::uint64_t>>,
                                    std::uint64_t,
                                    bioseq::parallelism::memory_permit permit) {
        permit.release();
        index_error("external sort spill is not supported by opportunistic storage");
    }

    /** Reports an index error because this operation is unsupported. */
    void merge_runs(auto&&) {
        index_error("external sort merge is not supported by opportunistic storage");
    }

    /** Does nothing because opportunistic fragment storage holds no sorted runs. */
    void release_resident_runs_noexcept() noexcept {}

    /** Does nothing because opportunistic fragment storage holds no sorted runs. */
    void release_runs_noexcept() noexcept {}

    /** Does nothing because opportunistic fragment storage holds no sorted runs. */
    void release_runs() {}

    /** Always returns zero because external sort is unsupported. */
    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return 0;
    }
};

// Transient minimizer-sequence storage backing a real no-index per-sequence
// build. Unlike opportunistic_fragment_index_storage (which refuses external-sort
// spill), this delegates spill/merge to a real sorted_pair_run_store so it satisfies
// is_external_sort_spill_storage + is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
// that direct_minimizer_bucket_index<p_sequence> requires. No persisted sidecar rows.
/** In-memory byte payload for one opportunistic per-sequence index chunk. */
struct opportunistic_sequence_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::size_t chunk_no = 0;
    std::vector<char> bytes;
};

struct opportunistic_sequence_index_storage {
    /** Key/value pair spilled during external sort. */
    using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;
    /** One position-segment ordinal and its independently mergeable run store. */
    using segment_run_entry =
        std::pair<std::uint32_t, bioseq::storage::indexing::sorted_pair_run_store>;

    std::vector<opportunistic_sequence_chunk> chunks;
    bioseq::storage::indexing::sorted_pair_run_store sorted_runs;
    // Permit precedes the vector so reverse destruction drops segment stores and
    // metadata capacity before the admission token.
    bioseq::parallelism::memory_permit segment_run_metadata_permit;
    std::vector<segment_run_entry> segment_runs;
    // Ordinal, never an iterator: segment-store insertion may relocate every entry.
    // The named store is the only one allowed to own the reusable spill writer.
    std::optional<std::uint32_t> active_segment_write_buffer_owner;
    std::size_t run_buffer_bytes_value = std::size_t{256} << 20;
    bioseq::indexing::direct_minimizer_spill_storage_binding segment_spill_binding{};
    inline static std::atomic<std::uint64_t> next_segment_generation_token{1u};

    /** Advance the mutation revision, invalidating an exhausted binding. */
    void record_segment_mutation() noexcept {
        if (!segment_spill_binding.valid()) {
            return;
        }
        if (segment_spill_binding.mutation_revision ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            segment_spill_binding = {};
            return;
        }
        ++segment_spill_binding.mutation_revision;
    }

    /** Release all position-segment runs through their checked cleanup path. */
    void release_segment_runs() {
        for (auto& entry : segment_runs) {
            entry.second.release_runs();
        }
        std::vector<segment_run_entry>{}.swap(segment_runs);
        if (segment_run_metadata_permit) {
            segment_run_metadata_permit.shrink_to(0u);
        }
        active_segment_write_buffer_owner.reset();
    }

    /** Returns the configured in-memory buffer budget for sorted runs. */
    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return run_buffer_bytes_value;
    }

    /** Clears chunk payloads and releases spilled sorted runs. */
    void clear_entity() {
        chunks.clear();
        sorted_runs.release_runs();
        release_segment_runs();
        record_segment_mutation();
    }

    // Required by is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
    // but unused on the transient path.
    void clear(std::string_view) {
        clear_entity();
    }

    /** Resizes or creates chunks for @p sub_key from @p chunk_plan. */
    void resize_chunks(bioseq::indexing::index_sub_key_t sub_key,
                       std::span<const std::size_t> chunk_plan) {
        for (std::size_t chunk_no = 0; chunk_no < chunk_plan.size(); ++chunk_no) {
            auto& chunk = ensure_chunk(sub_key, chunk_no);
            chunk.bytes.resize(chunk_plan[chunk_no]);
        }
    }

    /** Stores all @p bytes in a single chunk for @p sub_key. */
    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes) {
        std::array<std::size_t, 1> chunk_plan{bytes};
        resize_chunks(sub_key, std::span<const std::size_t>{chunk_plan.data(), chunk_plan.size()});
    }

    /** No-op prefetch hook required by the storage concept. */
    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    /** Invokes @p writer for each existing chunk of @p sub_key. */
    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        for (opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key != sub_key) {
                continue;
            }
            const std::size_t cap = chunk.bytes.size() / sizeof(p_value);
            writer(chunk.chunk_no, reinterpret_cast<p_value*>(chunk.bytes.data()), cap);
        }
    }

    /** Alias for @c write_to_each on the transient per-sequence path. */
    template <class p_value, class p_function>
    void write_to(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        write_to_each<p_value>(sub_key, std::forward<p_function>(writer));
    }

    /** Returns raw bytes for a chunk. */
    [[nodiscard]] const char* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                         std::size_t chunk_no) const {
        return chunk_ref(sub_key, chunk_no).bytes.data();
    }

    /** Returns a typed view into chunk bytes. */
    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                            std::size_t chunk_no) const {
        return reinterpret_cast<const p_value*>(chunk_ref(sub_key, chunk_no).bytes.data());
    }

    /** Returns stored byte length for a chunk. */
    [[nodiscard]] std::size_t chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key,
                                              std::size_t chunk_no) const {
        return chunk_ref(sub_key, chunk_no).bytes.size();
    }

    /** Returns how many chunks belong to @p sub_key. */
    [[nodiscard]] std::size_t number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t chunk_count = 0;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++chunk_count;
            }
        }
        return chunk_count;
    }

    // Reports the distinct sub-keys required by the storage concept.
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t sub_key_count = 0;
        for_each_sub_key([&sub_key_count](bioseq::indexing::index_sub_key_t) { ++sub_key_count; });
        return sub_key_count;
    }

    /** Invokes @p callback for each distinct sub-key. */
    template <class p_function> void for_each_sub_key(p_function&& callback) const {
        bioseq::indexing::index_sub_key_t last_sub_key = 0;
        bool have_last_sub_key = false;
        // chunks are appended in build order; emit each distinct sub_key once.
        std::vector<bioseq::indexing::index_sub_key_t> seen_sub_keys;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (have_last_sub_key && chunk.sub_key == last_sub_key) {
                continue;
            }
            bool already_seen = false;
            for (const auto key : seen_sub_keys) {
                if (key == chunk.sub_key) {
                    already_seen = true;
                    break;
                }
            }
            if (!already_seen) {
                seen_sub_keys.push_back(chunk.sub_key);
                callback(chunk.sub_key);
            }
            last_sub_key = chunk.sub_key;
            have_last_sub_key = true;
        }
    }

    /** Spills a sorted key/value span and returns the run index. */
    [[nodiscard]] std::size_t spill_sorted_run(std::span<const sorted_pair_t> sorted) {
        return sorted_runs.spill_sorted_run(sorted);
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(std::vector<sorted_pair_t> sorted) {
        // The transient no-index minimizer-sequence path immediately enters the next target
        // range. Keep these runs file-backed so resident pair
        // vectors do not consume the admission budget needed by the nested GPU
        // sketch dispatch.
        return sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
    }

    /** Spills an owned sorted run after admission and releases @p permit. */
    [[nodiscard]] std::size_t
    spill_sorted_run_owned_admitted(std::vector<sorted_pair_t> sorted,
                                    std::uint64_t admitted_bytes,
                                    bioseq::parallelism::memory_permit permit) {
        (void)admitted_bytes;
        const std::size_t run_index = sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
        permit.release();
        return run_index;
    }

    /** Merges spilled runs and forwards each k-mer to @p on_kmer. */
    template <class p_function> void merge_runs(p_function&& on_kmer) {
        sorted_runs.merge_runs(std::forward<p_function>(on_kmer));
    }

    /** Start a fresh nonzero identity for qualified per-segment replay. */
    [[nodiscard]] bioseq::indexing::direct_minimizer_spill_storage_binding
    begin_segment_spill_generation() {
        release_segment_runs();
        std::uint64_t token =
            next_segment_generation_token.load(std::memory_order_relaxed);
        for (;;) {
            if (token == 0u) {
                segment_spill_binding = {};
                return segment_spill_binding;
            }
            const std::uint64_t next =
                token == (std::numeric_limits<std::uint64_t>::max)() ? 0u : token + 1u;
            if (next_segment_generation_token.compare_exchange_weak(
                    token, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
        }
        segment_spill_binding = bioseq::indexing::direct_minimizer_spill_storage_binding{
            .generation_token = token,
            .mutation_revision = 0u,
        };
        return segment_spill_binding;
    }

    /** Return the current qualified-spill identity without transferring it. */
    [[nodiscard]] bioseq::indexing::direct_minimizer_spill_storage_binding
    segment_spill_storage_binding() const noexcept {
        return segment_spill_binding;
    }

    /** Spill one canonical run into its position-segment owner. */
    [[nodiscard]] std::size_t
    spill_sorted_segment_run(std::uint32_t position_segment,
                             std::span<const sorted_pair_t> sorted) {
        if (!segment_spill_binding.valid()) {
            (void)begin_segment_spill_generation();
        }
        if (!segment_spill_binding.valid()) {
            index_error("qualified segment spill generation token is exhausted");
        }
        auto iterator = std::lower_bound(
            segment_runs.begin(),
            segment_runs.end(),
            position_segment,
            [](const segment_run_entry& entry, std::uint32_t segment) {
                return entry.first < segment;
            });
        if (iterator == segment_runs.end() || iterator->first != position_segment) {
            const std::size_t offset =
                static_cast<std::size_t>(iterator - segment_runs.begin());
            if (!ensure_transient_vector_capacity(segment_run_metadata_permit,
                                                  segment_runs,
                                                  segment_runs.size() + 1u)) {
                index_error("qualified segment spill metadata admission failed");
            }
            iterator = segment_runs.insert(
                segment_runs.begin() + static_cast<std::ptrdiff_t>(offset),
                segment_run_entry{position_segment,
                                  bioseq::storage::indexing::sorted_pair_run_store{}});
        }
        if (!active_segment_write_buffer_owner.has_value()) {
            if (!iterator->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                index_error("qualified segment spill writer ownership is invalid");
            }
            active_segment_write_buffer_owner = position_segment;
        } else if (*active_segment_write_buffer_owner != position_segment) {
            // Destination metadata is admitted before transfer. Re-find the
            // source by ordinal because insertion may have relocated it.
            const auto source = std::lower_bound(
                segment_runs.begin(),
                segment_runs.end(),
                *active_segment_write_buffer_owner,
                [](const segment_run_entry& entry, std::uint32_t segment) {
                    return entry.first < segment;
                });
            if (source == segment_runs.end() ||
                source->first != *active_segment_write_buffer_owner ||
                !source->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                index_error("qualified segment spill writer transfer is invalid");
            }
            active_segment_write_buffer_owner = position_segment;
        }
        const std::size_t result = iterator->second.spill_sorted_run(sorted);
        record_segment_mutation();
        return result;
    }

    /** Merge every qualified segment in ordinal/hash order. */
    template <class p_function> void merge_segment_runs(p_function&& on_segment_kmer) {
        for (auto& entry : segment_runs) {
            entry.second.release_completed_write_buffer();
        }
        active_segment_write_buffer_owner.reset();
        for (auto& entry : segment_runs) {
            entry.second.merge_runs(
                [position_segment = entry.first,
                 &on_segment_kmer](std::uint64_t hash, auto&& positions) {
                    on_segment_kmer(position_segment,
                                    hash,
                                    std::forward<decltype(positions)>(positions));
                });
        }
    }

    /** Repartition canonical spill rows into a replacement position-segment span. */
    [[nodiscard]] bool reroute_segment_runs(std::uint64_t old_span,
                                            std::uint64_t new_span,
                                            std::uint64_t target_length) {
        if (!segment_spill_binding.valid() || old_span == 0u || new_span == 0u) {
            return false;
        }
        for (auto& entry : segment_runs) {
            entry.second.release_completed_write_buffer();
        }
        active_segment_write_buffer_owner.reset();
        struct admitted_segment_runs {
            bioseq::parallelism::memory_permit permit;
            std::vector<segment_run_entry> values;
        } replacement;
        std::optional<std::uint32_t> replacement_active_write_buffer_owner;
        struct admitted_pair_batch {
            bioseq::parallelism::memory_permit permit;
            std::vector<sorted_pair_t> values;
        } batch;
        const auto replacement_store =
            [this,
             &replacement,
             &replacement_active_write_buffer_owner](std::uint32_t segment)
                -> bioseq::storage::indexing::sorted_pair_run_store& {
            auto iterator = std::lower_bound(
                replacement.values.begin(),
                replacement.values.end(),
                segment,
                [](const segment_run_entry& entry, std::uint32_t value) {
                    return entry.first < value;
                });
            if (iterator == replacement.values.end() || iterator->first != segment) {
                const std::size_t offset =
                    static_cast<std::size_t>(iterator - replacement.values.begin());
                if (!ensure_transient_vector_capacity(replacement.permit,
                                                      replacement.values,
                                                      replacement.values.size() + 1u)) {
                    throw std::bad_alloc{};
                }
                iterator = replacement.values.insert(
                    replacement.values.begin() + static_cast<std::ptrdiff_t>(offset),
                    segment_run_entry{
                        segment, bioseq::storage::indexing::sorted_pair_run_store{}});
            }
            if (!replacement_active_write_buffer_owner.has_value()) {
                if (!iterator->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                    index_error("qualified segment reroute writer ownership is invalid");
                }
                replacement_active_write_buffer_owner = segment;
            } else if (*replacement_active_write_buffer_owner != segment) {
                const auto source = std::lower_bound(
                    replacement.values.begin(),
                    replacement.values.end(),
                    *replacement_active_write_buffer_owner,
                    [](const segment_run_entry& entry, std::uint32_t value) {
                        return entry.first < value;
                    });
                if (source == replacement.values.end() ||
                    source->first != *replacement_active_write_buffer_owner ||
                    !source->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                    index_error("qualified segment reroute writer transfer is invalid");
                }
                replacement_active_write_buffer_owner = segment;
            }
            return iterator->second;
        };
        bool valid = true;
        for (auto& old_entry : segment_runs) {
            old_entry.second.merge_runs(
                [old_segment = old_entry.first,
                 old_span,
                 new_span,
                 target_length,
                 &batch,
                 &replacement_store,
                 &valid](std::uint64_t hash, auto&& positions) {
                    positions.for_each_batch(
                        [old_segment,
                         old_span,
                         new_span,
                         target_length,
                         hash,
                         &batch,
                         &replacement_store,
                         &valid](std::span<const std::uint64_t> values) {
                            if (!valid) {
                                return;
                            }
                            std::size_t offset = 0u;
                            while (offset < values.size()) {
                                const std::uint64_t first_position = values[offset] >> 1u;
                                const std::uint64_t new_segment = first_position / new_span;
                                if (first_position >= target_length ||
                                    first_position / old_span != old_segment ||
                                    new_segment >
                                        (std::numeric_limits<std::uint32_t>::max)()) {
                                    valid = false;
                                    return;
                                }
                                std::size_t end = offset + 1u;
                                while (end < values.size()) {
                                    const std::uint64_t position = values[end] >> 1u;
                                    if (position >= target_length ||
                                        position / old_span != old_segment) {
                                        valid = false;
                                        return;
                                    }
                                    if (position / new_span != new_segment) {
                                        break;
                                    }
                                    ++end;
                                }
                                const std::size_t count = end - offset;
                                if (!ensure_transient_vector_capacity(
                                        batch.permit, batch.values, count)) {
                                    throw std::bad_alloc{};
                                }
                                batch.values.clear();
                                for (std::size_t index = offset; index < end; ++index) {
                                    batch.values.emplace_back(hash, values[index]);
                                }
                                replacement_store(static_cast<std::uint32_t>(new_segment))
                                    .spill_sorted_run(std::span<const sorted_pair_t>{batch.values});
                                offset = end;
                            }
                        });
                });
            if (!valid) {
                segment_spill_binding = {};
                return false;
            }
        }
        release_segment_runs();
        segment_runs.swap(replacement.values);
        std::swap(segment_run_metadata_permit, replacement.permit);
        active_segment_write_buffer_owner = replacement_active_write_buffer_owner;
        record_segment_mutation();
        return true;
    }

    /** Releases resident sorted-run buffers without throwing. */
    void release_resident_runs_noexcept() noexcept {
        sorted_runs.release_resident_runs_noexcept();
    }

    /** Releases all sorted runs and their backing storage. */
    void release_runs() {
        sorted_runs.release_runs();
        release_segment_runs();
        record_segment_mutation();
    }

    /** Releases all sorted runs without throwing. */
    void release_runs_noexcept() noexcept {
        sorted_runs.release_runs_noexcept();
        for (auto& entry : segment_runs) {
            entry.second.release_runs_noexcept();
        }
        std::vector<segment_run_entry>{}.swap(segment_runs);
        if (segment_run_metadata_permit) {
            segment_run_metadata_permit.shrink_to(0u);
        }
        active_segment_write_buffer_owner.reset();
        record_segment_mutation();
    }

    /** Invokes @p callback for every chunk in [@p range_begin, @p range_end). */
    template <class p_function>
    void read_chunks_in_range(bioseq::indexing::index_sub_key_t range_begin,
                              bioseq::indexing::index_sub_key_t range_end,
                              p_function&& callback) const {
        for (bioseq::indexing::index_sub_key_t sub_key = range_begin; sub_key < range_end;
             ++sub_key) {
            for (const opportunistic_sequence_chunk& chunk : chunks) {
                if (chunk.sub_key == sub_key) {
                    callback(sub_key, chunk.bytes.data(), chunk.bytes.size());
                }
            }
        }
    }

    /** Does nothing because the transient path does not use directory validation. */
    void validate_minimizer_directory_or_error(const char*, std::size_t) const {}

    /** Does nothing because the transient path does not use chunk metadata validation. */
    void validate_minimizer_chunk_metadata_or_error(bioseq::indexing::index_sub_key_t,
                                                    std::size_t,
                                                    std::size_t,
                                                    std::size_t) const {}

    /** Validates minimizer blob header size and optional bucket count. */
    auto validate_minimizer_blob_or_error(const char* blob,
                                          std::size_t bytes,
                                          std::uint32_t expected_bucket_count = 0) const {
        using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
        if (bytes < sizeof(header_type)) {
            index_error("opportunistic minimizer-sequence: truncated blob");
        }
        header_type header{};
        std::memcpy(&header, blob, sizeof(header));
        if (expected_bucket_count != 0u && header.bucket_count != expected_bucket_count) {
            index_error("opportunistic minimizer-sequence: unexpected bucket count");
        }
        return header;
    }

    /** Raises a PostgreSQL data-corruption error and does not return. */
    [[noreturn]] void index_error(std::string_view message) const {
        bioseq::postgres::support::invoke_pg_as_cpp([message] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq opportunistic minimizer-sequence index: %.*s",
                            static_cast<int>(message.size()),
                            message.data())));
        });
        pg_unreachable();
    }

  private:
    /** Returns an existing chunk or appends a zero-length placeholder. */
    opportunistic_sequence_chunk& ensure_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                               std::size_t chunk_no) {
        for (opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        chunks.push_back(opportunistic_sequence_chunk{
            .sub_key = sub_key,
            .chunk_no = chunk_no,
            .bytes = {},
        });
        return chunks.back();
    }

    /** Returns a chunk reference or reports a missing chunk. */
    [[nodiscard]] const opportunistic_sequence_chunk&
    chunk_ref(bioseq::indexing::index_sub_key_t sub_key, std::size_t chunk_no) const {
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        const std::string detail = std::format(
            "missing chunk sub_key={} chunk_no={}", static_cast<long long>(sub_key), chunk_no);
        index_error(detail);
    }
};

/** Address and byte range of one payload row retained in a contiguous transient image. */
struct transient_grouped_soa_payload_row {
    bioseq::indexing::index_chunk_address address{};
    std::uint64_t offset = 0u;
    std::uint32_t byte_count = 0u;
    std::uint64_t row_checksum = 0u;
};
static_assert(std::is_trivially_copyable_v<transient_grouped_soa_payload_row>);
static_assert(std::is_trivially_destructible_v<transient_grouped_soa_payload_row>);

/**
 * Statement-local grouped-SoA row sink for no-index minimizer-sequence generation.
 *
 * Biological payload bytes and the bounded row directory have independently allocator-measured
 * contiguous owners. Their separate authorities prevent a reflected payload from being charged
 * again while row metadata is retained.
 * Directory and manifest callbacks are authenticated by their address-bound checksums but are not
 * copied: the encoder's returned immutable directory/manifest already owns those bytes.
 */
class transient_grouped_soa_payload_storage {
  private:
    /** Stateless native-heap deleter paired with the audited allocation helper. */
    struct payload_deleter {
        void operator()(std::byte* allocation) const noexcept {
            std::free(allocation);
        }
    };

    /** Stateless native-heap deleter for the trivial payload-row array. */
    struct payload_row_deleter {
        void operator()(transient_grouped_soa_payload_row* allocation) const noexcept {
            std::free(allocation);
        }
    };

    // Each permit precedes only the allocation it accounts. Reverse destruction therefore drops
    // metadata, then its permit, and drops the payload before its retained permit. A payload
    // permit released by the materializer is represented by payload_capacity_bytes_, not reused
    // to admit metadata and accidentally charge the reflected payload a second time.
    bioseq::parallelism::memory_permit payload_permit_;
    std::unique_ptr<std::byte, payload_deleter> payload_{};
    bioseq::parallelism::memory_permit payload_rows_permit_;
    std::unique_ptr<transient_grouped_soa_payload_row, payload_row_deleter> payload_rows_{};
    std::optional<bioseq::indexing::grouped_soa_storage_plan_view> plan_;
    std::uint64_t payload_capacity_bytes_ = 0u;
    bool payload_permit_released_to_footprint_ = false;
    std::uint64_t payload_rows_capacity_bytes_ = 0u;
    std::size_t payload_row_count_ = 0u;
    std::size_t payload_row_capacity_ = 0u;
    bool payload_rows_permit_released_to_footprint_ = false;
    std::uint64_t payload_cursor_ = 0u;
    bioseq::indexing::crc64_ecma182_accumulator metadata_row_digest_{};
    std::uint64_t metadata_row_count_ = 0u;
    std::uint32_t next_directory_chunk_ = 1u;
    bool manifest_seen_ = false;
    bool completed_ = false;

    /** Return whether the payload has exactly one live admission/reflection authority. */
    [[nodiscard]] bool payload_accounting_valid_() const noexcept {
        if (payload_capacity_bytes_ == 0u) {
            return payload_ == nullptr && !payload_permit_ &&
                   !payload_permit_released_to_footprint_;
        }
        return payload_ != nullptr &&
               payload_permit_released_to_footprint_ != static_cast<bool>(payload_permit_);
    }

    /** Return whether the row array has exactly one live admission/reflection authority. */
    [[nodiscard]] bool payload_rows_accounting_valid_() const noexcept {
        if (payload_rows_capacity_bytes_ == 0u) {
            return payload_rows_ == nullptr && payload_row_capacity_ == 0u &&
                   payload_row_count_ == 0u && !payload_rows_permit_ &&
                   !payload_rows_permit_released_to_footprint_;
        }
        return payload_rows_ != nullptr && payload_row_capacity_ != 0u &&
               payload_row_count_ <= payload_row_capacity_ &&
               payload_rows_permit_released_to_footprint_ !=
                   static_cast<bool>(payload_rows_permit_);
    }

    /** Fold one metadata callback into a stable address/checksum transcript. */
    static void fold_metadata_row_(
        bioseq::indexing::crc64_ecma182_accumulator& digest,
        const bioseq::indexing::minimizer_generation_row_view& row) noexcept {
        digest.update_little_endian(row.address.generation_id);
        digest.update_little_endian(
            static_cast<std::uint32_t>(row.address.position_segment));
        digest.update_little_endian(static_cast<std::uint64_t>(row.address.sub_key));
        digest.update_little_endian(row.address.chunk_no);
        digest.update_little_endian(static_cast<std::uint64_t>(row.bytes.size()));
        digest.update_little_endian(row.row_checksum);
    }

  public:
    transient_grouped_soa_payload_storage() = default;
    transient_grouped_soa_payload_storage(const transient_grouped_soa_payload_storage&) = delete;
    transient_grouped_soa_payload_storage&
    operator=(const transient_grouped_soa_payload_storage&) = delete;
    transient_grouped_soa_payload_storage(transient_grouped_soa_payload_storage&&) = delete;
    transient_grouped_soa_payload_storage&
    operator=(transient_grouped_soa_payload_storage&&) = delete;

    /** Pre-admit and fully initialize the exact planned payload image. */
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    begin_planned_generation(
        const bioseq::indexing::grouped_soa_storage_plan_view& plan) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (plan_.has_value() || completed_ || plan.layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            plan.generation_id == 0u ||
            plan.payload_bytes >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
            plan.file_bytes < plan.payload_bytes ||
            plan.directory_offset > plan.file_bytes ||
            plan.manifest_offset > plan.file_bytes) {
            return std::unexpected(error::storage_contract_mismatch);
        }
        if (plan.payload_bytes == 0u) {
            plan_ = plan;
            return {};
        }
        bioseq::common::heap_allocation_detail::heap_allocation_plan allocation_plan{};
        try {
            allocation_plan = bioseq::common::heap_allocation_detail::plan_heap_allocation(
                static_cast<std::size_t>(plan.payload_bytes));
        } catch (const std::overflow_error&) {
            return std::unexpected(error::arithmetic_overflow);
        } catch (const std::exception&) {
            return std::unexpected(error::unsupported_storage_layout);
        }
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        auto admitted = admission.try_reserve(
            static_cast<std::uint64_t>(allocation_plan.allocation_requirement_bytes));
        if (!admitted) {
            return std::unexpected(error::admission);
        }
        payload_permit_ = std::move(*admitted);
        bioseq::parallelism::memory_materialization_result materialization{};
        try {
            materialization = admission.materialize(
                payload_permit_,
                [this, allocation_plan]() {
                    std::size_t actual = 0u;
                    void* allocation =
                        bioseq::common::heap_allocation_detail::allocate_and_touch(
                            allocation_plan, actual);
                    if (allocation == nullptr || actual == 0u ||
                        actual > payload_permit_.bytes()) {
                        throw std::bad_alloc{};
                    }
                    payload_.reset(static_cast<std::byte*>(allocation));
                    payload_permit_.shrink_to(static_cast<std::uint64_t>(actual));
                    return bioseq::parallelism::memory_materialization_snapshot{
                        .actual_allocation_bytes = static_cast<std::uint64_t>(actual),
                        .fully_touched_bytes = static_cast<std::uint64_t>(actual),
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = true,
                    };
                });
        } catch (...) {
            payload_.reset();
            payload_permit_.release();
            throw;
        }
        const std::uint64_t physical_payload_bytes =
            materialization.allocation_snapshot.actual_allocation_bytes;
        if (materialization.admitted_bytes != physical_payload_bytes ||
            physical_payload_bytes < plan.payload_bytes ||
            materialization.is_permit_released == static_cast<bool>(payload_permit_) ||
            bioseq::common::heap_allocation_detail::allocator_reported_bytes(payload_.get()) !=
                physical_payload_bytes) {
            payload_.reset();
            payload_permit_.release();
            return std::unexpected(error::admission);
        }
        payload_capacity_bytes_ = physical_payload_bytes;
        payload_permit_released_to_footprint_ = materialization.is_permit_released;

        constexpr std::uint64_t directory_entry_bytes = 128u;
        static_assert(bioseq::indexing::minimizer_directory_entry_encoded_bytes(
                          bioseq::indexing::grouped_soa_minimizer_profile{}) ==
                      directory_entry_bytes);
        constexpr std::uint64_t payload_extent_bytes =
            bioseq::indexing::k_grouped_soa_extent_payload_bytes;
        if (payload_extent_bytes == 0u ||
            plan.directory_bytes % directory_entry_bytes != 0u) {
            return std::unexpected(error::storage_contract_mismatch);
        }
        const std::uint64_t directory_entries =
            plan.directory_bytes / directory_entry_bytes;
        const std::uint64_t payload_rows_from_bytes = bioseq::common::ceil_div(
            plan.payload_bytes, payload_extent_bytes);
        const std::uint64_t extent_boundary_rows = bioseq::common::saturating_mul(
            directory_entries, 2u);
        const std::uint64_t maximum_payload_rows = bioseq::common::saturating_add(
            payload_rows_from_bytes, extent_boundary_rows);
        const std::uint64_t payload_row_bytes = bioseq::common::saturating_mul(
            maximum_payload_rows,
            static_cast<std::uint64_t>(sizeof(transient_grouped_soa_payload_row)));
        if (extent_boundary_rows == (std::numeric_limits<std::uint64_t>::max)() ||
            maximum_payload_rows == (std::numeric_limits<std::uint64_t>::max)() ||
            payload_row_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            maximum_payload_rows >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
            payload_row_bytes >
                static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return std::unexpected(error::arithmetic_overflow);
        }
        if (payload_row_bytes != 0u) {
            bioseq::common::heap_allocation_detail::heap_allocation_plan row_allocation_plan{};
            try {
                row_allocation_plan =
                    bioseq::common::heap_allocation_detail::plan_heap_allocation(
                        static_cast<std::size_t>(payload_row_bytes));
            } catch (const std::overflow_error&) {
                return std::unexpected(error::arithmetic_overflow);
            } catch (const std::exception&) {
                return std::unexpected(error::unsupported_storage_layout);
            }
            auto row_permit = admission.try_reserve(
                static_cast<std::uint64_t>(row_allocation_plan.allocation_requirement_bytes));
            if (!row_permit) {
                return std::unexpected(error::admission);
            }
            payload_rows_permit_ = std::move(*row_permit);
            const auto row_materialization = admission.materialize(
                payload_rows_permit_,
                [this, row_allocation_plan]() {
                    std::size_t actual = 0u;
                    void* allocation =
                        bioseq::common::heap_allocation_detail::allocate_and_touch(
                            row_allocation_plan, actual);
                    if (allocation == nullptr || actual == 0u ||
                        actual > payload_rows_permit_.bytes()) {
                        throw std::bad_alloc{};
                    }
                    payload_rows_.reset(
                        static_cast<transient_grouped_soa_payload_row*>(allocation));
                    payload_rows_permit_.shrink_to(static_cast<std::uint64_t>(actual));
                    return bioseq::parallelism::memory_materialization_snapshot{
                        .actual_allocation_bytes = static_cast<std::uint64_t>(actual),
                        .fully_touched_bytes = static_cast<std::uint64_t>(actual),
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = true,
                    };
                });
            payload_rows_capacity_bytes_ =
                row_materialization.allocation_snapshot.actual_allocation_bytes;
            payload_row_capacity_ = static_cast<std::size_t>(maximum_payload_rows);
            payload_rows_permit_released_to_footprint_ =
                row_materialization.is_permit_released;
            if (row_materialization.admitted_bytes != payload_rows_capacity_bytes_ ||
                payload_rows_capacity_bytes_ < payload_row_bytes ||
                bioseq::common::heap_allocation_detail::allocator_reported_bytes(
                    payload_rows_.get()) != payload_rows_capacity_bytes_ ||
                !payload_rows_accounting_valid_()) {
                return std::unexpected(error::admission);
            }
        }
        plan_ = plan;
        return {};
    }

    /** Copy one biological payload row and return its address-bound checksum. */
    [[nodiscard]] std::expected<bioseq::indexing::minimizer_generation_row_write_receipt,
                                bioseq::indexing::minimizer_generation_encoding_error>
    write_grouped_soa_payload(
        const bioseq::indexing::unchecked_minimizer_generation_row_view& row) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (!plan_.has_value() || completed_ || manifest_seen_ ||
            !payload_accounting_valid_() ||
            row.address.generation_id != plan_->generation_id ||
            row.address.position_segment < 0 ||
            row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(error::row_order);
        }
        const auto end = bioseq::common::checked_add(
            payload_cursor_, static_cast<std::uint64_t>(row.bytes.size()));
        if (!end || *end > plan_->payload_bytes) {
            return std::unexpected(error::byte_count);
        }
        if (!payload_accounting_valid_() || !payload_rows_accounting_valid_() ||
            payload_row_count_ == payload_row_capacity_) {
            return std::unexpected(error::admission);
        }
        std::span<std::byte> full_payload{
            payload_.get(), static_cast<std::size_t>(plan_->payload_bytes)};
        std::span<std::byte> destination = full_payload.subspan(
            static_cast<std::size_t>(payload_cursor_), row.bytes.size());
        std::ranges::copy(row.bytes, destination.begin());
        const std::uint64_t row_checksum =
            bioseq::indexing::crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                destination);
        payload_rows_.get()[payload_row_count_] = transient_grouped_soa_payload_row{
            .address = row.address,
            .offset = payload_cursor_,
            .byte_count = static_cast<std::uint32_t>(row.bytes.size()),
            .row_checksum = row_checksum,
        };
        ++payload_row_count_;
        payload_cursor_ = *end;
        return bioseq::indexing::minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }

    /** Authenticate directory/manifest callback order without retaining duplicate bytes. */
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (!plan_.has_value() || completed_ || row.address.position_segment != -1 ||
            row.address.generation_id != plan_->generation_id ||
            row.address.sub_key != bioseq::indexing::k_directory_sub_key) {
            return std::unexpected(error::row_order);
        }
        if (row.address.chunk_no == 0u) {
            if (manifest_seen_) {
                return std::unexpected(error::row_order);
            }
            manifest_seen_ = true;
        } else if (manifest_seen_ || row.address.chunk_no != next_directory_chunk_) {
            return std::unexpected(error::row_order);
        } else {
            if (next_directory_chunk_ ==
                (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(error::arithmetic_overflow);
            }
            ++next_directory_chunk_;
        }
        if (row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(error::byte_count);
        }
        const std::uint64_t expected_checksum = bioseq::indexing::crc64_index_chunk_row(
            row.address.generation_id,
            row.address.position_segment,
            row.address.sub_key,
            row.address.chunk_no,
            static_cast<std::uint32_t>(row.bytes.size()),
            row.bytes);
        if (expected_checksum != row.row_checksum ||
            metadata_row_count_ == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(error::checksum);
        }
        fold_metadata_row_(metadata_row_digest_, row);
        ++metadata_row_count_;
        return {};
    }

    /** Verify the complete retained image and the metadata callback transcript. */
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    complete(const bioseq::indexing::encoded_grouped_soa_generation& generation) {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (!plan_.has_value() || completed_ || !manifest_seen_ ||
            !payload_accounting_valid_() || !payload_rows_accounting_valid_() ||
            payload_cursor_ != plan_->payload_bytes ||
            generation.manifest.layout != plan_->layout ||
            generation.manifest.generation_id != plan_->generation_id ||
            generation.manifest.payload_bytes != plan_->payload_bytes ||
            generation.manifest.directory_bytes != plan_->directory_bytes ||
            static_cast<std::uint64_t>(generation.manifest.directory_chunk_count) + 1u !=
                static_cast<std::uint64_t>(next_directory_chunk_)) {
            return std::unexpected(error::storage_contract_mismatch);
        }
        if (auto metadata = bioseq::indexing::validate_minimizer_generation_metadata(
                generation.head, generation.manifest, generation.directory);
            !metadata) {
            return std::unexpected(metadata.error());
        }
        auto validator = bioseq::indexing::minimizer_generation_row_validator::create(generation);
        if (!validator) {
            return std::unexpected(validator.error());
        }
        std::span<const std::byte> full_payload{
            payload_.get(), static_cast<std::size_t>(plan_->payload_bytes)};
        for (const transient_grouped_soa_payload_row& stored :
             std::span<const transient_grouped_soa_payload_row>{payload_rows_.get(),
                                                                 payload_row_count_}) {
            if (stored.offset > full_payload.size() ||
                stored.byte_count > full_payload.size() - stored.offset) {
                return std::unexpected(error::byte_count);
            }
            const auto row = bioseq::indexing::minimizer_generation_row_view{
                .address = stored.address,
                .bytes = full_payload.subspan(static_cast<std::size_t>(stored.offset),
                                              stored.byte_count),
                .row_checksum = stored.row_checksum,
            };
            if (auto accepted = validator->accept(row); !accepted) {
                return std::unexpected(accepted.error());
            }
        }

        bioseq::indexing::crc64_ecma182_accumulator expected_metadata_digest{};
        std::uint64_t expected_metadata_rows = 0u;
        const std::uint32_t extent_bytes =
            bioseq::indexing::minimizer_extent_payload_bytes(generation.manifest.layout);
        const std::span<const std::byte> directory = generation.directory.encoded_bytes();
        if (extent_bytes == 0u) {
            return std::unexpected(error::unsupported_storage_layout);
        }
        for (std::size_t offset = 0u, chunk = 1u; offset < directory.size();
             offset += extent_bytes, ++chunk) {
            const std::size_t count =
                std::min<std::size_t>(extent_bytes, directory.size() - offset);
            const auto bytes = directory.subspan(offset, count);
            const bioseq::indexing::index_chunk_address address{
                .generation_id = generation.manifest.generation_id,
                .position_segment = -1,
                .sub_key = bioseq::indexing::k_directory_sub_key,
                .chunk_no = static_cast<std::uint32_t>(chunk),
            };
            const auto row = bioseq::indexing::minimizer_generation_row_view{
                .address = address,
                .bytes = bytes,
                .row_checksum = bioseq::indexing::crc64_index_chunk_row(
                    address.generation_id,
                    address.position_segment,
                    address.sub_key,
                    address.chunk_no,
                    static_cast<std::uint32_t>(bytes.size()),
                    bytes),
            };
            fold_metadata_row_(expected_metadata_digest, row);
            ++expected_metadata_rows;
            if (auto accepted = validator->accept(row); !accepted) {
                return std::unexpected(accepted.error());
            }
        }
        const auto manifest_row = bioseq::indexing::minimizer_generation_row_view{
            .address = generation.manifest_row.address,
            .bytes = generation.manifest_row.bytes(),
            .row_checksum = generation.manifest_row.row_checksum,
        };
        fold_metadata_row_(expected_metadata_digest, manifest_row);
        ++expected_metadata_rows;
        if (auto accepted = validator->accept(manifest_row); !accepted) {
            return std::unexpected(accepted.error());
        }
        if (expected_metadata_rows != metadata_row_count_ ||
            expected_metadata_digest.checksum() != metadata_row_digest_.checksum()) {
            return std::unexpected(error::checksum);
        }
        if (auto finished = validator->finish_ready(); !finished) {
            return std::unexpected(finished.error());
        }
        completed_ = true;
        return {};
    }

    /** Visit each retained payload row through its stable contiguous backing image. */
    template <class p_callback>
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    for_each_payload_row(p_callback&& callback) const {
        using error = bioseq::indexing::minimizer_generation_encoding_error;
        if (!completed_ || !plan_.has_value() || !payload_accounting_valid_() ||
            !payload_rows_accounting_valid_()) {
            return std::unexpected(error::row_order);
        }
        const std::span<const std::byte> full_payload{
            payload_.get(), static_cast<std::size_t>(plan_->payload_bytes)};
        for (const transient_grouped_soa_payload_row& stored :
             std::span<const transient_grouped_soa_payload_row>{payload_rows_.get(),
                                                                 payload_row_count_}) {
            if (stored.offset > full_payload.size() ||
                stored.byte_count > full_payload.size() - stored.offset) {
                return std::unexpected(error::byte_count);
            }
            const auto row = bioseq::indexing::minimizer_generation_row_view{
                .address = stored.address,
                .bytes = full_payload.subspan(static_cast<std::size_t>(stored.offset),
                                              stored.byte_count),
                .row_checksum = stored.row_checksum,
            };
            if (auto accepted = std::invoke(callback, row); !accepted) {
                return std::unexpected(accepted.error());
            }
        }
        return {};
    }

    /** Return allocator-reported payload and row-array capacity. */
    [[nodiscard]] std::uint64_t retained_capacity_bytes() const noexcept {
        const std::uint64_t retained =
            bioseq::common::saturating_add(payload_capacity_bytes_,
                                           payload_rows_capacity_bytes_);
        if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
            !payload_accounting_valid_() || !payload_rows_accounting_valid_()) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return retained;
    }
};

/** Builds a persist target for a per-fragment minimizer index row. */
template <class p_scanner_pg_context>
[[nodiscard]] bioseq::postgres::scheduler::persist_target make_fragment_index_persist_target(
    const p_scanner_pg_context& ctx,
    const bioseq::storage::sequence_locator& loc,
    std::uint64_t sequence_rowno,
    std::string_view index_name = bioseq::indexing::minimizer_hashmap_fragment_index::index_name) {
    return bioseq::postgres::scheduler::persist_target{
        .db_oid = ctx.db_oid,
        .user_relid = ctx.user_relid,
        .tablespace_oid = ctx.tablespace_oid,
        .main_relfilenode = ctx.main_relfilenode,
        .column_attno = ctx.mmsearch_seq_attno,
        .bucket_idx = static_cast<std::uint8_t>(loc.bucket_index),
        .shard_idx = loc.shard_index,
        .sequence_rowno = sequence_rowno,
        .index_name = index_name,
    };
}

/** True when async recompute may defer minimizer persistence for this scan context. */
template <class p_scanner_pg_context>
[[nodiscard]] bool
can_defer_recompute_minimizer_persist(const p_scanner_pg_context& ctx,
                                      const minimap2_parameters& params) noexcept {
    return ctx.recompute_async_persist_active && ctx.index_async_persist_active &&
           OidIsValid(ctx.user_relid) && params.kmer_size == 15 &&
           params.window_size == 10
           // Existing asynchronous minimizer-fragment/minimizer-sequence payloads
           // have no minimizer-scheme field.
           // Bounded ntHash recompute remains bounded, but persistence must wait for the
           // scheme-tagged READY-generation writer; never relabel it Exact/scalar.
           && pipe::target_index_sketch_backend_from_params(params) !=
                  bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd;
}

// Select the target index sketch backend for recompute from
// the resolved params (Vulkan only when all three high-level gates are on).
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
target_backend_from_params_for_recompute(const minimap2_parameters& params) noexcept {
    return pipe::target_index_sketch_backend_from_params(params);
}

/** Resolves segment unit ranges for PostgreSQL indexing using the selected sketch backend. */
[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
pg_segment_unit_ranges(sequence_size_t total_bp,
                       sequence_size_t query_len,
                       std::size_t lpu,
                       const minimap2_parameters& params) {
    const auto backend = ((params.vulkan_enabled && params.vulkan_seed_hits_enabled) ||
                          target_backend_from_params_for_recompute(params) ==
                              bioseq::minimizers::minimizer_sketch_backend::vulkan_segment)
                             ? bioseq::minimizers::minimizer_sketch_backend::vulkan_segment
                             : bioseq::minimizers::minimizer_sketch_backend::scalar;
    return pipe::segment_unit_ranges_for_backend(total_bp, query_len, lpu, backend);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
