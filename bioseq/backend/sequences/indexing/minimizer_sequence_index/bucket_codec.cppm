/**
 * @file minimizer_sequence_index/bucket_codec.cppm
 * @brief Direct minimizer bucket wire records and serialization.
 */
module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

export module bioseq.indexing.minimizer_sequence:bucket_codec;

import bioseq.common.checked_arithmetic;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.sequences.kmers;
import bioseq.storage.size;

export namespace bioseq::indexing {

using bioseq::minimizers::minimizer_sketch_backend;
using bioseq::storage::sequence_size_t;

/**
 * @brief Records scalar collection work for one direct-bucket build.
 *
 * Every collection leaf records the symbols and packed bytes actually handed
 * to `collect_minimizers_view`. Parallel builds merge worker-local instances
 * after the join in range order.
 */
struct direct_minimizer_bucket_build_stats {
    minimizer_sketch_backend selected_backend = minimizer_sketch_backend::scalar;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint32_t letters_per_storage_unit = 0;
    std::uint64_t scalar_collection_calls = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t completed_builds = 0;
    bool backend_bound = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    /**
     * @brief Adds a counter increment with saturation reporting.
     * @param destination Counter to update
     * @param increment Increment to add
     * @param saturated_flag Set when the addition overflows
     */
    static constexpr void add(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& saturated_flag) noexcept {
        const auto sum = bioseq::common::checked_add(destination, increment);
        if (!sum) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination = *sum;
    }

    /**
     * @brief Binds the receipt to one backend and packed storage shape.
     * @param backend Selected sketch backend
     * @param storage_unit_bytes Bytes per packed storage unit
     * @param letters_per_unit Logical symbols per storage unit
     */
    constexpr void bind_backend(
            minimizer_sketch_backend backend,
            std::uint32_t storage_unit_bytes,
            std::uint32_t letters_per_unit) noexcept {
        if (storage_unit_bytes == 0 || letters_per_unit == 0) {
            aggregation_mismatch = true;
        }
        if (!backend_bound) {
            selected_backend = backend;
            source_storage_unit_bytes = storage_unit_bytes;
            letters_per_storage_unit = letters_per_unit;
            backend_bound = true;
            return;
        }
        aggregation_mismatch = aggregation_mismatch
            || selected_backend != backend
            || source_storage_unit_bytes != storage_unit_bytes
            || letters_per_storage_unit != letters_per_unit;
    }

    /**
     * @brief Records one scalar collection call.
     * @param symbols Logical symbols consumed
     * @param storage_units Packed storage units consumed
     * @param storage_unit_bytes Bytes per storage unit
     * @param letters_per_unit Logical symbols per storage unit
     */
    constexpr void record_scalar_collection(
            std::uint64_t symbols,
            std::uint64_t storage_units,
            std::uint32_t storage_unit_bytes,
            std::uint32_t letters_per_unit) noexcept {
        bind_backend(
            minimizer_sketch_backend::scalar,
            storage_unit_bytes,
            letters_per_unit);
        bool overflow = saturated;
        add(scalar_collection_calls, 1u, overflow);
        add(input_symbols, symbols, overflow);
        add(input_storage_units, storage_units, overflow);
        std::uint64_t bytes = 0;
        const auto byte_count = bioseq::common::checked_mul(
            storage_units, static_cast<std::uint64_t>(storage_unit_bytes));
        if (!byte_count) {
            bytes = (std::numeric_limits<std::uint64_t>::max)();
            overflow = true;
        } else {
            bytes = *byte_count;
        }
        add(input_bytes, bytes, overflow);
        saturated = overflow;
    }

    /**
     * @brief Merges another worker-local receipt.
     * @param other Receipt to merge
     */
    constexpr void merge(
            const direct_minimizer_bucket_build_stats& other) noexcept {
        const bool other_has_counters = other.scalar_collection_calls != 0
            || other.input_symbols != 0
            || other.input_storage_units != 0
            || other.input_bytes != 0
            || other.completed_builds != 0;
        if (!other.backend_bound && !other_has_counters
            && !other.aggregation_mismatch && !other.saturated) {
            return;
        }
        if (other.backend_bound) {
            bind_backend(
                other.selected_backend,
                other.source_storage_unit_bytes,
                other.letters_per_storage_unit);
        } else if (other_has_counters) {
            aggregation_mismatch = true;
        }
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch;
        bool overflow = saturated || other.saturated;
        add(scalar_collection_calls, other.scalar_collection_calls, overflow);
        add(input_symbols, other.input_symbols, overflow);
        add(input_storage_units, other.input_storage_units, overflow);
        add(input_bytes, other.input_bytes, overflow);
        add(completed_builds, other.completed_builds, overflow);
        saturated = overflow;
    }

    /** @brief Records successful completion of the direct-bucket build. */
    constexpr void record_completed_build() noexcept {
        add(completed_builds, 1u, saturated);
    }

    /**
     * @brief Validates backend binding and operation-originated counters.
     * @param require_nonempty_work Whether the build must have consumed input
     * @return `true` when the receipt is internally consistent
     */
    [[nodiscard]] constexpr bool is_internally_consistent(
            bool require_nonempty_work = true) const noexcept {
        if (!backend_bound || selected_backend != minimizer_sketch_backend::scalar
            || aggregation_mismatch || saturated || completed_builds != 1
            || source_storage_unit_bytes == 0
            || letters_per_storage_unit == 0
            || scalar_collection_calls == 0
            || input_storage_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / source_storage_unit_bytes
            || input_bytes
                != input_storage_units * source_storage_unit_bytes
            || input_storage_units
                > (std::numeric_limits<std::uint64_t>::max)()
                    / letters_per_storage_unit
            || input_symbols
                > input_storage_units * letters_per_storage_unit) {
            return false;
        }
        return !require_nonempty_work
            || (input_symbols > 0 && input_bytes > 0
                && scalar_collection_calls <= input_symbols);
    }

    [[nodiscard]] constexpr bool operator==(
        const direct_minimizer_bucket_build_stats&) const noexcept = default;
};

/** @brief Hash interval and storage key for one direct minimizer bucket. */
struct direct_minimizer_bucket_directory_entry {
    /// @brief Inclusive smallest hash in the bucket.
    std::uint64_t lo_kmer;
    /// @brief Inclusive largest hash in the bucket.
    std::uint64_t hi_kmer;
    /// @brief Persisted storage sub-key of the bucket.
    std::int64_t sub_key;
};
static_assert(sizeof(direct_minimizer_bucket_directory_entry) == 24 &&
              alignof(direct_minimizer_bucket_directory_entry) == 8);

/**
 * @brief Fixed persisted directory header followed by sorted bucket entries.
 *
 * Field widths, offsets, magic, and layout revision are serialization ABI.
 */
struct direct_minimizer_bucket_directory {
    /// @brief Magic word identifying the direct-bucket directory wire image.
    static constexpr std::uint32_t k_direct_minimizer_bucket_magic = 0xB1000002u;
    /// @brief Current direct-bucket directory layout revision.
    static constexpr std::uint32_t k_direct_minimizer_bucket_layout_revision = 2u;
    /// @brief Flag recording generation through the Vulkan backend.
    static constexpr std::uint32_t k_gpu_built = 1u << 0u;
    /// @brief Persisted magic word.
    std::uint32_t magic = k_direct_minimizer_bucket_magic;
    /// @brief Persisted layout revision.
    std::uint32_t layout_revision = k_direct_minimizer_bucket_layout_revision;
    /// @brief Persisted generation flags.
    std::uint32_t flags = 0;
    /// @brief Reserved field that remains zero in revision 2.
    std::uint32_t reserved = 0;
    /// @brief Number of directory entries following this header.
    std::uint64_t num_buckets = 0;
    /// @brief Total distinct minimizer hashes across all buckets.
    bioseq::storage::sequence_size_t total_distinct_kmers = 0;
    /// @brief Total encoded positions across all buckets.
    bioseq::storage::sequence_size_t total_positions = 0;
};
static_assert(sizeof(direct_minimizer_bucket_directory) == 40 &&
                  alignof(direct_minimizer_bucket_directory) == 8,
              "directory wire header layout pinned");
static_assert(sizeof(bioseq::storage::sequence_size_t) == 8,
              "directory wire fields assume 8-byte sequence_size_t");
static_assert(offsetof(direct_minimizer_bucket_directory, flags) == 8 &&
                  offsetof(direct_minimizer_bucket_directory, reserved) == 12 &&
                  offsetof(direct_minimizer_bucket_directory, num_buckets) == 16 &&
                  offsetof(direct_minimizer_bucket_directory, total_distinct_kmers) == 24 &&
                  offsetof(direct_minimizer_bucket_directory, total_positions) == 32,
              "directory header field offsets pinned");
static_assert(offsetof(direct_minimizer_bucket_directory_entry, sub_key) == 16,
              "dir entry field offsets pinned");

/**
 * @brief Owning aligned directory view supporting binary-search lookup.
 *
 * The source blob is copied so subsequent storage reads cannot invalidate the
 * decoded header or entry pointers.
 */
struct direct_minimizer_bucket_directory_view {
    /// @brief Aligned storage retaining the copied wire bytes.
    std::vector<std::int64_t> owned;
    /// @brief Decoded header within `owned`.
    const direct_minimizer_bucket_directory* hdr = nullptr;
    /// @brief First decoded entry within `owned`.
    const direct_minimizer_bucket_directory_entry* entries = nullptr;
    /// @brief Number of decoded entries.
    std::size_t n = 0;

    /**
     * @brief Copies and validates one persisted directory image.
     * @param blob Persisted bytes
     * @param bytes Persisted byte count
     */
    direct_minimizer_bucket_directory_view(const char* blob, std::size_t bytes) {
        if (bytes < sizeof(direct_minimizer_bucket_directory)) {
            index_corruption_panic("directory: truncated");
        }
        owned.resize((bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t));
        std::memcpy(owned.data(), blob, bytes);
        hdr = reinterpret_cast<const direct_minimizer_bucket_directory*>(owned.data());
        if (hdr->magic != direct_minimizer_bucket_directory::k_direct_minimizer_bucket_magic ||
            hdr->layout_revision !=
                direct_minimizer_bucket_directory::k_direct_minimizer_bucket_layout_revision) {
            index_corruption_panic("directory: magic/layout revision");
        }
        n = static_cast<std::size_t>(hdr->num_buckets);
        const std::size_t expected_bytes = sizeof(direct_minimizer_bucket_directory) +
                                           sizeof(direct_minimizer_bucket_directory_entry) * n;
        if (bytes != expected_bytes) {
            index_corruption_panic("directory: size mismatch");
        }
        entries = reinterpret_cast<const direct_minimizer_bucket_directory_entry*>(
            reinterpret_cast<const char*>(owned.data()) +
            sizeof(direct_minimizer_bucket_directory));
        for (std::size_t index = 1; index < n; ++index) {
            if (entries[index].lo_kmer <= entries[index - 1].hi_kmer) {
                index_corruption_panic("directory: not ascending/non-overlapping");
            }
        }
    }
    /**
     * @brief Finds the bucket covering one minimizer hash.
     * @param kmer Minimizer hash
     * @return Matching directory entry or `nullptr`
     */
    [[nodiscard]] const direct_minimizer_bucket_directory_entry* find(std::uint64_t kmer) const {
        std::size_t lo = 0, hi = n;
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (entries[mid].hi_kmer < kmer) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < n && entries[lo].lo_kmer <= kmer && kmer <= entries[lo].hi_kmer) {
            return &entries[lo];
        }
        return nullptr;
    }
    /**
     * @brief Reports whether the persisted directory records Vulkan generation.
     * @return `true` when the Vulkan flag is set
     */
    [[nodiscard]] bool is_gpu_built() const noexcept {
        return hdr != nullptr
            && (hdr->flags & direct_minimizer_bucket_directory::k_gpu_built) != 0u;
    }
};

namespace detail {

using encoded_bucket_header = minimizer_hashmap_fragment_index::encoded_header;
using encoded_bucket_record = minimizer_hashmap_fragment_index::encoded_bucket;

constexpr std::size_t k_encoded_header_bytes = minimizer_hashmap_fragment_index::k_encoded_header_bytes;     // 32
constexpr std::size_t k_encoded_bucket_bytes = minimizer_hashmap_fragment_index::k_encoded_bucket_bytes;     // 16

/**
 * @brief Serializes a direct-bucket header, records, and flat positions.
 * @param out Destination buffer sized for the complete image
 * @param target_len_val Target length stored in the header
 * @param bkts Bucket records
 * @param poss Flat encoded positions
 */
inline void serialize_direct_minimizer_bucket_image(char* out,
                                                    sequence_size_t target_len_val,
                                                    std::span<const encoded_bucket_record> bkts,
                                                    std::span<const std::uint64_t> poss) {
    const encoded_bucket_header header{minimizer_hashmap_fragment_index::k_encoded_magic,
                                       minimizer_hashmap_fragment_index::k_encoded_layout_revision,
                                       static_cast<std::uint64_t>(target_len_val),
                                       static_cast<std::uint32_t>(bkts.size()),
                                       0u,
                                       static_cast<std::uint64_t>(poss.size())};
    std::memcpy(out, &header, k_encoded_header_bytes);
    if (!bkts.empty()) {
        std::memcpy(
            out + k_encoded_header_bytes, bkts.data(), k_encoded_bucket_bytes * bkts.size());
    }
    if (!poss.empty()) {
        std::memcpy(out + k_encoded_header_bytes + k_encoded_bucket_bytes * bkts.size(),
                    poss.data(),
                    sizeof(std::uint64_t) * poss.size());
    }
}

} // namespace detail

} // namespace bioseq::indexing
