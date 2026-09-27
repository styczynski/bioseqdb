/**
 * @file hash_major_minimizer_index/reader/contract.cppm
 * @brief Source callbacks and lookup interface for selected hashes in a stored index.
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

/** @brief Posting byte range selected from a checked control block for one rexd batch. */
struct hash_major_minimizer_authorized_posting_range {
    std::uint64_t control_block_ordinal = 0u;
    std::uint64_t posting_byte_offset = 0u;
    std::uint64_t byte_count = 0u;
    std::uint64_t expected_posting_crc64 = 0u;
    std::uint32_t request_begin = 0u;
    std::uint32_t request_count = 0u;
    std::uint32_t flags = 0u; // Bit 0 means this  range covers the whole block and its CRC can be checked.
    std::uint32_t reserved_zero = 0u;

    [[nodiscard]] bool
    operator==(const hash_major_minimizer_authorized_posting_range&) const = default;
};
static_assert(sizeof(hash_major_minimizer_authorized_posting_range) == 48u);

/** @brief Control block selected from the checked directory for one read batch. */
struct hash_major_minimizer_authorized_control_range {
    std::uint64_t control_block_ordinal = 0u;
    std::uint64_t control_byte_offset = 0u;
    std::uint32_t byte_count = 0u;
    std::uint32_t reserved_zero = 0u;

    [[nodiscard]] bool
    operator==(const hash_major_minimizer_authorized_control_range&) const = default;
};
static_assert(sizeof(hash_major_minimizer_authorized_control_range) == 24u);

/// Consume a posting fragment identified by its range number and byte offset
using hash_major_minimizer_consume_posting_bytes_fn = bool (*)(void*,
                                                               std::uint32_t,
                                                               std::uint64_t,
                                                               std::span<const std::byte>);
/// Read a batch of posting ranges and deliver their fragments in order to the consumer.
using hash_major_minimizer_read_posting_wave_fn =
    bool (*)(void*,
             std::span<const hash_major_minimizer_authorized_posting_range>,
             void*,
             hash_major_minimizer_consume_posting_bytes_fn);
/// Consume one complete control block identified by its range number.
using hash_major_minimizer_consume_control_block_fn = bool (*)(void*,
                                                               std::uint32_t,
                                                               std::span<const std::byte>);
/// Read a batch of control ranges and deliver each complete block in order.
using hash_major_minimizer_read_control_wave_fn =
    bool (*)(void*,
             std::span<const hash_major_minimizer_authorized_control_range>,
             void*,
             hash_major_minimizer_consume_control_block_fn);
/// Read an exact byte interval from one  logical stream.
using hash_major_minimizer_read_logical_bytes_fn = bool (*)(void*,
                                                            hash_major_minimizer_stream_kind,
                                                            std::uint64_t,
                                                            std::span<std::byte>);
/// Callback that returns one logical stream's exact byte size.
using hash_major_minimizer_logical_stream_size_fn =
    std::uint64_t (*)(void*, hash_major_minimizer_stream_kind);

/** @brief Borrowed source callbacks for exact byte reads and optional batched reads. */
struct hash_major_minimizer_logical_stream_source_ref {
    void* context = nullptr;
    hash_major_minimizer_read_logical_bytes_fn read_bytes = nullptr;
    hash_major_minimizer_logical_stream_size_fn logical_size = nullptr;
    hash_major_minimizer_read_control_wave_fn read_control_wave = nullptr;
    hash_major_minimizer_read_posting_wave_fn read_posting_wave = nullptr;

    /// Read the requested byte interval, returning false if the callback is missing or fails.
    [[nodiscard]] bool read(hash_major_minimizer_stream_kind kind,
                            std::uint64_t offset,
                            std::span<std::byte> output) const {
        return read_bytes != nullptr && read_bytes(context, kind, offset, output);
    }

    /// Return a logical stream's declared byte length, or zero without a size callback.
    [[nodiscard]] std::uint64_t size(hash_major_minimizer_stream_kind kind) const {
        return logical_size == nullptr ? 0u : logical_size(context, kind);
    }
};

/**
 * @brief Load counts and selected positions for a caller's query hashes.
 *
 * Opening checks the stored generation and reads the control blocks selected
 * by the query hashes. It saves positions whose counts pass max_occ. Query
 * hashes remain borrowed for the reader lifetime, while its saved arrays and
 * temporary read buffers are covered by its memory permit.
 */
class hash_major_minimizer_selective_reader {
  private:
    /// Checked posting bounds and checksum kept unil a selected block has been read.
    struct block_authority {
        std::uint64_t directory_ordinal = 0u;
        std::uint64_t posting_byte_offset = 0u;
        std::uint64_t posting_byte_length = 0u;
        std::uint64_t posting_crc64 = 0u;
    };
    static_assert(sizeof(block_authority) == 32u);

    /// Opening stages used  to report peak reserved memory.
    enum class memory_admission_stage : std::uint8_t {
        directory,
        control,
        posting,
    };

    inline static constexpr std::uint64_t k_absent_query_offset =
        (std::numeric_limits<std::uint64_t>::max)();
    inline static constexpr std::uint64_t k_unavailable_query_offset =
        (std::numeric_limits<std::uint64_t>::max)() - 1u;

    /// Progress, and checksum state shared by callbacks delivering one batch of positions.
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

    /// Query cursor and validation state shared by callbacks delivering control blocks.
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

    // Declare the permit first so arrays are freed before their memory
    // reservation is released.
    bioseq::parallelism::memory_permit permit_{};
    hash_major_minimizer_manifest manifest_{};
    canonical_query_hash_view query_hashes_{}; // The caller keeps these hashes alive and unchanged.
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

    /// Compute the memory held by a vector's capacity, rejecting overflow
    /// @tparam p_value_type Element type used to calculate the byte count.
    template <class p_value_type>
    [[nodiscard]] static bool vector_capacity_bytes_(const std::vector<p_value_type>& values,
                                                     std::uint64_t& output) noexcept {
        return hash_major_minimizer_detail::checked_multiply(
            values.capacity(), sizeof(p_value_type), output);
    }

    /// Recount the memory held by the reader's arrays for permit  accounting.
    [[nodiscard]] bool refresh_persistent_owner_bytes_() noexcept {
        using namespace hash_major_minimizer_detail;
        std::uint64_t total = 0u;
        // Add this allocation to the total, rejecting overflow.
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

    /// Record the highest reservation seen in this stage and during the whole open.
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

    /// Increase the permit to cover these bytes and update the stage peak.
    [[nodiscard]] bool grow_permit_to_(std::uint64_t bytes,
                                       memory_admission_stage admission_stage) noexcept {
        if (!permit_.grow_to(bytes))
            return false;
        record_admitted_peak_(admission_stage, permit_.bytes());
        return true;
    }

    /// Read a query's exact count, including the exception for a saturated 16-bit count.
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

    /// Select the radix bucket used to narrow the search for this hash.
    [[nodiscard]] std::size_t canonical_high16_bucket_(std::uint64_t hash) const noexcept {
        // Use bits above bit 15 for four-byte hashes. Eight-byte hashes use
        // bits above bit 23, fitting the 65536 buckets for a 40-bit hash.
        const unsigned shift =
            manifest_.storage_profile.hash_width_bytes == sizeof(std::uint64_t) ? 24u : 16u;
        return static_cast<std::size_t>(hash >> shift);
    }

    /// Find a prepared hash's index  by searching its radiy bucket.
    [[nodiscard]] std::size_t canonical_ordinal_(std::uint64_t hash) const noexcept {
        if (hash > hash_major_minimizer_max_hash(manifest_.storage_profile) ||
            canonical_radix_prefix_.size() != 65'537u) {
            return query_hashes_.size();
        }
        const std::size_t radix = canonical_high16_bucket_(hash);
        if (radix + 1u >= canonical_radix_prefix_.size()) {
            return query_hashes_.size();
        }
        const std::size_t begin = canonical_radix_prefix_[radix];
        const std::size_t end = canonical_radix_prefix_[radix + 1u];
        const std::size_t found = query_hashes_.lower_bound(begin, end, hash);
        return found < end && query_hashes_.at(found) == hash ? found : query_hashes_.size();
    }

    /// Find the first query hash at least as large as hash, starting at first.
    [[nodiscard]] std::size_t canonical_lower_bound_(std::size_t first,
                                                     std::uint64_t hash) const noexcept {
        if (hash > hash_major_minimizer_max_hash(manifest_.storage_profile) ||
            canonical_radix_prefix_.size() != 65'537u) {
            return query_hashes_.size();
        }
        const std::size_t radix = canonical_high16_bucket_(hash);
        if (radix + 1u >= canonical_radix_prefix_.size()) {
            return query_hashes_.size();
        }
        const std::size_t bucket_begin = canonical_radix_prefix_[radix];
        const std::size_t bucket_end = canonical_radix_prefix_[radix + 1u];
        if (first >= bucket_end)
            return first;
        return query_hashes_.lower_bound(std::max(first, bucket_begin), bucket_end, hash);
    }

    /// Check whether the hash belongs to the caller's prepared query set.
    [[nodiscard]] bool canonical_contains_(std::uint64_t hash) const noexcept {
        return canonical_ordinal_(hash) != query_hashes_.size();
    }

    /// Check one delivered control block and record matching query counts and offsets.
    [[nodiscard]] static bool consume_control_block_(void* opaque,
                                                     std::uint32_t range_ordinal,
                                                     std::span<const std::byte> encoded_control);

    /// Check one ordered posting fragment and copy its positions into the reader's arrays.
    [[nodiscard]] static bool consume_posting_bytes_(void* opaque,
                                                     std::uint32_t range_ordinal,
                                                     std::uint64_t range_byte_offset,
                                                     std::span<const std::byte> bytes);

  public:
    hash_major_minimizer_selective_reader() = default;
    hash_major_minimizer_selective_reader(const hash_major_minimizer_selective_reader&) = delete;
    hash_major_minimizer_selective_reader&
    operator=(const hash_major_minimizer_selective_reader&) = delete;
    /// Move the arrays and, memory permit while preserving the borrowed query spans.
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
     * @brief Open a stored generation for sorted, unique query hashes.
     *
     * Keep  the query hashes alive and unchanged for the reader lifetime Source
     * callbacks and encoded metadata are needed only during this call. The
     * reader reserves memory before allocating its saved arrays and read
     * buffers, and checks the source against the expected generation.
     *
     * @param admission Controller for saved arrays and temporary memory.
     * @param encoded_head Stored head record to validate.
     * @param encoded_manifest Manifest selected by the head, also validated here.
     * @param expected Required generation, source identity, and storage settings.
     * @param source Byte-read callbacks with optional batched control and posting reads.
     * @param query_hashes Sorted  unique hashes with the expected width and profile dighst.
     * @param max_occ Save positions when their global count is at most this value.
     * @return The prepared reader, or a validation, read, or memory error.
     */
    [[nodiscard]] static std::expected<hash_major_minimizer_selective_reader,
                                       hash_major_minimizer_error>
    open(bioseq::parallelism::memory_admission_controller& admission,
         std::span<const std::byte> encoded_head,
         std::span<const std::byte> encoded_manifest,
         const expected_hash_major_minimizer_generation& expected,
         hash_major_minimizer_logical_stream_source_ref source,
         canonical_query_hash_view query_hashes,
         std::uint64_t max_occ);

    /// Look up a prepared hash. Return its saved positions, or its exact count
    /// with positions marked unavailable when the count exceeded max_occ.
    /// A prepared hash absent from the stored index returns an empty result.
    ///
    /// @throws persisted_minimizer_lookup_error If the hash was not prepared or stored lookup state is inconsistent.
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

    /// Return the target  length recorded in the checked manifest.
    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return manifest_.target_length;
    }

    /// Borrow saved four-byte encoded positions, or an empty span for an eight-byte profile.
    [[nodiscard]] std::span<const std::uint32_t> retained_positions32() const noexcept {
        return {retained_positions32_.get(), retained_positions32_ ? retained_position_count_ : 0u};
    }

    /// Borrow saved eight-byte encoded positions, or an empty span for a four-byte profile
    [[nodiscard]] std::span<const std::uint64_t> retained_positions64() const noexcept {
        return {retained_positions64_.get(), retained_positions64_ ? retained_position_count_ : 0u};
    }

    /// Borrow exact counts that replaced saturated 16-bit primary counts.
    [[nodiscard]] std::span<const exact_global_count_exception>
    exact_count_exceptions() const noexcept {
        return count_exceptions_;
    }

    /// Return the number of prepared hashes present in the generation.
    [[nodiscard]] std::size_t selected_hash_count() const noexcept {
        return selected_hash_count_;
    }

    /// Check whether this hash was prepared, even if it has no stored occurrences.
    [[nodiscard]] bool contains_prepared_hash(std::uint64_t hash) const noexcept {
        return canonical_contains_(hash);
    }

    /// Return the peak bytes held by posting request and range arrays during open.
    [[nodiscard]] std::size_t posting_wave_peak_owner_bytes() const noexcept {
        return posting_wave_peak_owner_bytes_;
    }

    /// Return the peak bytes held by the control range array during open.
    [[nodiscard]] std::size_t control_wave_peak_owner_bytes() const noexcept {
        return control_wave_peak_owner_bytes_;
    }

    /// Return the peak bytes held by temporary checked posting bounds.
    [[nodiscard]] std::size_t transient_authority_peak_owner_bytes() const noexcept {
        return transient_authority_peak_owner_bytes_;
    }

    /// Opening frees all  temporary block-bouid records before returning.
    [[nodiscard]] static constexpr std::size_t transient_authority_retained_bytes() noexcept {
        return 0u;
    }

    /// Return the bytes still reserved by the reader's memory permit.
    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    /// Borrow the memory, checksum, selection, and delivery counters.
    [[nodiscard]] const hash_major_minimizer_memory_stats& memory_stats() const noexcept {
        return memory_stats_;
    }
};

static_assert(segmented_minimizer_lookup_index<hash_major_minimizer_selective_reader>);
static_assert(minimizer_lookup_index<hash_major_minimizer_selective_reader>);

} // namespace bioseq::indexing
