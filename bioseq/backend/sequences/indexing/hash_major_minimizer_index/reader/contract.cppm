/**
 * @file hash_major_minimizer_index/reader/contract.cppm
 * @brief Public selective-reader source callbacks, ownership, and lookup contract.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.indexing.hash_major_minimizer_index:reader_contract;

import :codec;
import :planner;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.parallelism.memory_admission;

export namespace bioseq::indexing {

/** @brief Authenticated posting byte range delivered as part of one read wave. */
struct hash_major_minimizer_authorized_posting_range {
    std::uint64_t control_block_ordinal = 0u;
    std::uint64_t posting_byte_offset = 0u;
    std::uint64_t byte_count = 0u;
    std::uint64_t expected_posting_crc64 = 0u;
    std::uint32_t request_begin = 0u;
    std::uint32_t request_count = 0u;
    std::uint32_t flags = 0u; // bit 0: complete authoritative block
    std::uint32_t reserved_zero = 0u;

    [[nodiscard]] bool
    operator==(const hash_major_minimizer_authorized_posting_range&) const = default;
};
static_assert(sizeof(hash_major_minimizer_authorized_posting_range) == 48u);

/** @brief Authenticated control-block range delivered as part of one read wave. */
struct hash_major_minimizer_authorized_control_range {
    std::uint64_t control_block_ordinal = 0u;
    std::uint64_t control_byte_offset = 0u;
    std::uint32_t byte_count = 0u;
    std::uint32_t reserved_zero = 0u;

    [[nodiscard]] bool
    operator==(const hash_major_minimizer_authorized_control_range&) const = default;
};
static_assert(sizeof(hash_major_minimizer_authorized_control_range) == 24u);

/// Callback that consumes a bounded posting fragment for one authorized range.
using hash_major_minimizer_consume_posting_bytes_fn = bool (*)(void*,
                                                               std::uint32_t,
                                                               std::uint64_t,
                                                               std::span<const std::byte>);
/// Callback that batches authenticated posting-range reads.
using hash_major_minimizer_read_posting_wave_fn =
    bool (*)(void*,
             std::span<const hash_major_minimizer_authorized_posting_range>,
             void*,
             hash_major_minimizer_consume_posting_bytes_fn);
/// Callback that consumes one complete authenticated control block.
using hash_major_minimizer_consume_control_block_fn = bool (*)(void*,
                                                               std::uint32_t,
                                                               std::span<const std::byte>);
/// Callback that batches authenticated control-block reads.
using hash_major_minimizer_read_control_wave_fn =
    bool (*)(void*,
             std::span<const hash_major_minimizer_authorized_control_range>,
             void*,
             hash_major_minimizer_consume_control_block_fn);
/// Generic logical-stream read callback used when no batch route is available.
using hash_major_minimizer_read_logical_bytes_fn = bool (*)(void*,
                                                            hash_major_minimizer_stream_kind,
                                                            std::uint64_t,
                                                            std::span<std::byte>);
/// Callback that returns one logical stream's exact byte size.
using hash_major_minimizer_logical_stream_size_fn =
    std::uint64_t (*)(void*, hash_major_minimizer_stream_kind);

/** @brief Non-owning callback table for logical and authenticated batch reads. */
struct hash_major_minimizer_logical_stream_source_ref {
    void* context = nullptr;
    hash_major_minimizer_read_logical_bytes_fn read_bytes = nullptr;
    hash_major_minimizer_logical_stream_size_fn logical_size = nullptr;
    hash_major_minimizer_read_control_wave_fn read_control_wave = nullptr;
    hash_major_minimizer_read_posting_wave_fn read_posting_wave = nullptr;

    [[nodiscard]] bool read(hash_major_minimizer_stream_kind kind,
                            std::uint64_t offset,
                            std::span<std::byte> output) const {
        return read_bytes != nullptr && read_bytes(context, kind, offset, output);
    }

    [[nodiscard]] std::uint64_t size(hash_major_minimizer_stream_kind kind) const {
        return logical_size == nullptr ? 0u : logical_size(context, kind);
    }
};

/**
 * @brief Memory-admitted selective hash-major reader with bounded cold reads.
 *
 * Query hashes are borrowed for the reader lifetime. Persisted bytes remain
 * owned by the callback source, and all retained decoded state is charged to
 * the shared memory-admission permit.
 */
class hash_major_minimizer_selective_reader {
  private:
    struct block_authority {
        std::uint64_t directory_ordinal = 0u;
        std::uint64_t posting_byte_offset = 0u;
        std::uint64_t posting_byte_length = 0u;
        std::uint64_t posting_crc64 = 0u;
    };
    static_assert(sizeof(block_authority) == 32u);

    enum class memory_admission_stage : std::uint8_t {
        directory,
        control,
        posting,
    };

    inline static constexpr std::uint64_t k_absent_query_offset =
        (std::numeric_limits<std::uint64_t>::max)();
    inline static constexpr std::uint64_t k_unavailable_query_offset =
        (std::numeric_limits<std::uint64_t>::max)() - 1u;

    struct posting_delivery_context {
        hash_major_minimizer_selective_reader* reader = nullptr;
        const hash_major_minimizer_manifest* manifest = nullptr;
        const std::vector<posting_request>* requests = nullptr;
        const std::vector<hash_major_minimizer_authorized_posting_range>* ranges = nullptr;
        std::size_t next_range = 0u;
        std::size_t next_request = 0u;
        std::uint64_t next_range_byte = 0u;
        crc64_ecma182_accumulator range_crc{};
        crc64_ecma182_accumulator* posting_stream_crc = nullptr;
        std::uint64_t* posting_stream_next = nullptr;
        bool* posting_stream_complete = nullptr;
        std::optional<hash_major_minimizer_error> error{};
    };

    struct control_delivery_context {
        hash_major_minimizer_selective_reader* reader = nullptr;
        const hash_major_minimizer_manifest* manifest = nullptr;
        canonical_query_hash_view query_hashes{};
        const std::vector<hash_major_minimizer_directory_entry>* directory = nullptr;
        std::vector<block_authority>* authorities = nullptr;
        const std::vector<hash_major_minimizer_authorized_control_range>* ranges = nullptr;
        hash_major_minimizer_control_scratch* scratch = nullptr;
        crc64_ecma182_accumulator* control_stream_crc = nullptr;
        std::uint64_t max_occ = 0u;
        std::size_t query_cursor = 0u;
        std::size_t next_range = 0u;
        std::size_t controls_read = 0u;
        std::optional<hash_major_minimizer_error> error{};
    };

    // The permit is declared before every admitted owner so reverse member
    // destruction frees vector capacity before returning its reservation.
    bioseq::parallelism::memory_permit permit_{};
    hash_major_minimizer_manifest manifest_{};
    canonical_query_hash_view query_hashes_{}; // caller lifetime is explicit
    std::vector<std::uint16_t> primary_counts_{};
    std::vector<exact_global_count_exception> count_exceptions_{};
    std::vector<std::uint64_t> query_offsets_{};
    std::unique_ptr<std::uint32_t[]> retained_positions32_{};
    std::unique_ptr<std::uint64_t[]> retained_positions64_{};
    std::size_t retained_position_count_ = 0u;
    std::vector<std::uint32_t> canonical_radix_prefix_{};
    hash_major_minimizer_memory_stats memory_stats_{};
    std::uint64_t persistent_owner_bytes_ = 0u;
    std::size_t selected_hash_count_ = 0u;
    std::size_t control_wave_peak_owner_bytes_ = 0u;
    std::size_t posting_wave_peak_owner_bytes_ = 0u;
    std::size_t transient_authority_peak_owner_bytes_ = 0u;

    template <class p_value_type>
    [[nodiscard]] static bool vector_capacity_bytes_(const std::vector<p_value_type>& values,
                                                     std::uint64_t& output) noexcept {
        return hash_major_minimizer_detail::checked_multiply(
            values.capacity(), sizeof(p_value_type), output);
    }

    [[nodiscard]] bool refresh_persistent_owner_bytes_() noexcept {
        using namespace hash_major_minimizer_detail;
        std::uint64_t total = 0u;
        const auto add = [&total](std::uint64_t bytes) { return checked_add(total, bytes, total); };
        std::uint64_t bytes = 0u;
        if (!vector_capacity_bytes_(primary_counts_, bytes) || !add(bytes) ||
            !vector_capacity_bytes_(count_exceptions_, bytes) || !add(bytes) ||
            !vector_capacity_bytes_(query_offsets_, bytes) || !add(bytes) ||
            !checked_multiply(retained_position_count_,
                              manifest_.storage_profile.stored_posting_width_bytes,
                              bytes) ||
            !add(bytes) || !vector_capacity_bytes_(canonical_radix_prefix_, bytes) || !add(bytes)) {
            return false;
        }
        persistent_owner_bytes_ = total;
        return true;
    }

    void record_admitted_peak_(memory_admission_stage admission_stage,
                               std::uint64_t bytes) noexcept {
        memory_stats_.peak_admitted_bytes = std::max(memory_stats_.peak_admitted_bytes, bytes);
        std::uint64_t* stage_peak = nullptr;
        switch (admission_stage) {
        case memory_admission_stage::directory:
            stage_peak = &memory_stats_.directory_stage_peak_admitted_bytes;
            break;
        case memory_admission_stage::control:
            stage_peak = &memory_stats_.control_stage_peak_admitted_bytes;
            break;
        case memory_admission_stage::posting:
            stage_peak = &memory_stats_.posting_stage_peak_admitted_bytes;
            break;
        }
        *stage_peak = std::max(*stage_peak, bytes);
    }

    [[nodiscard]] bool grow_permit_to_(std::uint64_t bytes,
                                       memory_admission_stage admission_stage) noexcept {
        if (!permit_.grow_to(bytes))
            return false;
        record_admitted_peak_(admission_stage, permit_.bytes());
        return true;
    }

    [[nodiscard]] std::uint64_t exact_count_(std::size_t query_ordinal) const noexcept {
        const std::uint16_t primary = primary_counts_[query_ordinal];
        if (primary != 65'535u)
            return primary;
        const auto found = std::lower_bound(
            count_exceptions_.begin(),
            count_exceptions_.end(),
            query_ordinal,
            [](const exact_global_count_exception& exception, std::uint32_t ordinal) {
                return exception.query_ordinal < ordinal;
            });
        return found != count_exceptions_.end() && found->query_ordinal == query_ordinal
                   ? found->exact_count
                   : 0u;
    }

    [[nodiscard]] std::size_t canonical_ordinal_(std::uint64_t hash) const noexcept {
        if (hash > (std::numeric_limits<std::uint32_t>::max)() ||
            canonical_radix_prefix_.size() != 65'537u) {
            return query_hashes_.size();
        }
        const std::size_t radix = static_cast<std::size_t>(hash >> 16u);
        const std::size_t begin = canonical_radix_prefix_[radix];
        const std::size_t end = canonical_radix_prefix_[radix + 1u];
        const std::size_t found = query_hashes_.lower_bound(begin, end, hash);
        return found < end && query_hashes_.at(found) == hash ? found : query_hashes_.size();
    }

    [[nodiscard]] std::size_t canonical_lower_bound_(std::size_t first,
                                                     std::uint64_t hash) const noexcept {
        if (hash > (std::numeric_limits<std::uint32_t>::max)() ||
            canonical_radix_prefix_.size() != 65'537u) {
            return query_hashes_.size();
        }
        const std::size_t radix = static_cast<std::size_t>(hash >> 16u);
        const std::size_t bucket_begin = canonical_radix_prefix_[radix];
        const std::size_t bucket_end = canonical_radix_prefix_[radix + 1u];
        if (first >= bucket_end)
            return first;
        return query_hashes_.lower_bound(std::max(first, bucket_begin), bucket_end, hash);
    }

    [[nodiscard]] bool canonical_contains_(std::uint64_t hash) const noexcept {
        return canonical_ordinal_(hash) != query_hashes_.size();
    }

    [[nodiscard]] static inline bool consume_control_block_(
        void* opaque, std::uint32_t range_ordinal, std::span<const std::byte> encoded_control);

    [[nodiscard]] static inline bool consume_posting_bytes_(void* opaque,
                                                            std::uint32_t range_ordinal,
                                                            std::uint64_t range_byte_offset,
                                                            std::span<const std::byte> bytes);

  public:
    hash_major_minimizer_selective_reader() = default;
    hash_major_minimizer_selective_reader(const hash_major_minimizer_selective_reader&) = delete;
    hash_major_minimizer_selective_reader&
    operator=(const hash_major_minimizer_selective_reader&) = delete;
    hash_major_minimizer_selective_reader(hash_major_minimizer_selective_reader&& other) noexcept
        : permit_(std::move(other.permit_)),
          manifest_(std::exchange(other.manifest_, hash_major_minimizer_manifest{})),
          query_hashes_(std::exchange(other.query_hashes_, canonical_query_hash_view{})),
          primary_counts_(std::move(other.primary_counts_)),
          count_exceptions_(std::move(other.count_exceptions_)),
          query_offsets_(std::move(other.query_offsets_)),
          retained_positions32_(std::move(other.retained_positions32_)),
          retained_positions64_(std::move(other.retained_positions64_)),
          retained_position_count_(std::exchange(other.retained_position_count_, 0u)),
          canonical_radix_prefix_(std::move(other.canonical_radix_prefix_)),
          memory_stats_(std::exchange(other.memory_stats_, hash_major_minimizer_memory_stats{})),
          persistent_owner_bytes_(std::exchange(other.persistent_owner_bytes_, 0u)),
          selected_hash_count_(std::exchange(other.selected_hash_count_, 0u)),
          control_wave_peak_owner_bytes_(std::exchange(other.control_wave_peak_owner_bytes_, 0u)),
          posting_wave_peak_owner_bytes_(std::exchange(other.posting_wave_peak_owner_bytes_, 0u)),
          transient_authority_peak_owner_bytes_(
              std::exchange(other.transient_authority_peak_owner_bytes_, 0u)) {}
    hash_major_minimizer_selective_reader&
    operator=(hash_major_minimizer_selective_reader&&) noexcept = delete;

    /**
     * Open against a caller-owned sorted-unique canonical image. The caller
     * keeps that image alive for the reader lifetime. No query hash is copied,
     * widened, sorted, or deduplicated here.
     */
    [[nodiscard]] static inline std::expected<hash_major_minimizer_selective_reader,
                                              hash_major_minimizer_error>
    open(bioseq::parallelism::memory_admission_controller& admission,
         std::span<const std::byte> encoded_head,
         std::span<const std::byte> encoded_manifest,
         const expected_hash_major_minimizer_generation& expected,
         hash_major_minimizer_logical_stream_source_ref source,
         canonical_query_hash_view query_hashes,
         std::uint64_t max_occ);

    [[nodiscard]] minimizer_lookup_spans lookup_spans(std::uint64_t hash) const {
        const std::size_t query_ordinal = canonical_ordinal_(hash);
        if (query_ordinal == query_hashes_.size()) {
            throw persisted_minimizer_lookup_error{
                "Hash-major lookup hash is absent from the authenticated "
                "canonical query image"};
        }
        const std::uint16_t primary = primary_counts_[query_ordinal];
        if (primary == 0u) {
            return {};
        }
        const std::uint64_t count = exact_count_(query_ordinal);
        if (count == 0u) {
            throw persisted_minimizer_lookup_error{"Hash-major exact-count exception is missing"};
        }
        const std::uint64_t offset = query_offsets_[query_ordinal];
        if (offset == k_unavailable_query_offset) {
            return minimizer_lookup_spans::positions_unavailable(count);
        }
        if (offset == k_absent_query_offset || offset > retained_position_count_ ||
            count > retained_position_count_ - offset) {
            throw persisted_minimizer_lookup_error{
                "Hash-major retained-position offset is inconsistent"};
        }
        const std::size_t position_offset = static_cast<std::size_t>(offset);
        const std::size_t position_count = static_cast<std::size_t>(count);
        if (manifest_.storage_profile.stored_posting_width_bytes == sizeof(std::uint32_t)) {
            return minimizer_lookup_spans::contiguous(
                std::span<const std::uint32_t>{retained_positions32_.get(),
                                               retained_position_count_}
                    .subspan(position_offset, position_count));
        }
        return minimizer_lookup_spans::contiguous(
            std::span<const std::uint64_t>{retained_positions64_.get(), retained_position_count_}
                .subspan(position_offset, position_count));
    }

    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return manifest_.target_length;
    }

    [[nodiscard]] std::span<const std::uint32_t> retained_positions32() const noexcept {
        return {retained_positions32_.get(), retained_positions32_ ? retained_position_count_ : 0u};
    }

    [[nodiscard]] std::span<const std::uint64_t> retained_positions64() const noexcept {
        return {retained_positions64_.get(), retained_positions64_ ? retained_position_count_ : 0u};
    }

    [[nodiscard]] std::span<const exact_global_count_exception>
    exact_count_exceptions() const noexcept {
        return count_exceptions_;
    }

    [[nodiscard]] std::size_t selected_hash_count() const noexcept {
        return selected_hash_count_;
    }

    [[nodiscard]] bool contains_prepared_hash(std::uint64_t hash) const noexcept {
        return canonical_contains_(hash);
    }

    [[nodiscard]] std::size_t posting_wave_peak_owner_bytes() const noexcept {
        return posting_wave_peak_owner_bytes_;
    }

    [[nodiscard]] std::size_t control_wave_peak_owner_bytes() const noexcept {
        return control_wave_peak_owner_bytes_;
    }

    [[nodiscard]] std::size_t transient_authority_peak_owner_bytes() const noexcept {
        return transient_authority_peak_owner_bytes_;
    }

    [[nodiscard]] static constexpr std::size_t transient_authority_retained_bytes() noexcept {
        return 0u;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] const hash_major_minimizer_memory_stats& memory_stats() const noexcept {
        return memory_stats_;
    }
};

static_assert(segmented_minimizer_lookup_index<hash_major_minimizer_selective_reader>);
static_assert(minimizer_lookup_index<hash_major_minimizer_selective_reader>);

} // namespace bioseq::indexing
