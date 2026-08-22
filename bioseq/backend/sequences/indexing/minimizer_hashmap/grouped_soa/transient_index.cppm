/**
 * @file minimizer_hashmap/grouped_soa/transient_index.cppm
 * @brief Owns the transient minimizer hash table and fixed bucket codec.
 */
export module bioseq.indexing.minimizer_hashmap:grouped_soa_transient_index;

import std;
import :exact_simd;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import :grouped_soa_contracts;
import :grouped_soa_codecs;

namespace bioseq::indexing::detail {
struct minimizer_hashmap_fragment_index_table;
}

export namespace bioseq::indexing {

/// Exact allocation bound for the transient flat hash table.
struct minimizer_hashmap_table_allocation_plan {
    /// Maximum number of distinct hashes accepted by the plan.
    std::uint64_t distinct_upper_bound = 0u;
    /// Slot count requested before group rounding.
    std::uint64_t requested_slots = 0u;
    /// Power-of-two group count allocated by the table.
    std::uint64_t group_count = 0u;
    /// Usable slot count after reserving the sentinel slot.
    std::uint64_t capacity = 0u;
    /// Bytes retained by the planned groups.
    std::uint64_t allocation_bytes = 0u;
    /// Whether every intermediate and the final allocation fit.
    bool valid = false;
};

/// Plan the table capacity and retained bytes for a distinct-hash upper bound.
[[nodiscard]] constexpr minimizer_hashmap_table_allocation_plan
plan_minimizer_hashmap_table_allocation(std::uint64_t possible_distinct) noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    constexpr std::uint64_t sentinel_margin = 64u;
    constexpr std::uint64_t load_numerator = 7u;
    constexpr std::uint64_t load_denominator = 8u;
    constexpr std::uint64_t slots_per_group = 15u;
    constexpr std::uint64_t bytes_per_group = 256u;
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();

    // An empty sketch builds no table at all.  Keep that real lifetime in the
    // exact plan instead of charging the non-empty sentinel margin and then
    // rejecting the builder's zero retained bytes at handoff validation.
    if (possible_distinct == 0u) {
        return minimizer_hashmap_table_allocation_plan{.valid = true};
    }

    const std::uint64_t bounded_distinct = saturating_add(possible_distinct, sentinel_margin);
    if (bounded_distinct == max)
        return {};
    const std::uint64_t quotient = bounded_distinct / load_numerator;
    const std::uint64_t remainder = bounded_distinct % load_numerator;
    const std::uint64_t requested_slots =
        saturating_add(saturating_mul(quotient, load_denominator),
                       (remainder * load_denominator + load_numerator - 1u) / load_numerator);
    if (requested_slots == max)
        return {};
    const std::uint64_t groups_needed = requested_slots / slots_per_group + 1u;
    std::uint64_t groups = 2u;
    while (groups < groups_needed) {
        if (groups > max / 2u)
            return {};
        groups *= 2u;
    }
    const std::uint64_t capacity = saturating_mul(groups, slots_per_group);
    const std::uint64_t allocation_bytes = saturating_mul(groups, bytes_per_group);
    if (capacity == max || allocation_bytes == max || capacity == 0u) {
        return {};
    }
    return minimizer_hashmap_table_allocation_plan{
        .distinct_upper_bound = bounded_distinct,
        .requested_slots = requested_slots,
        .group_count = groups,
        .capacity = capacity - 1u,
        .allocation_bytes = allocation_bytes,
        .valid = true,
    };
}

/// Transient storage-free minimizer lookup used by in-memory seed routes.
struct minimizer_hashmap_fragment_index {
  private:
    std::unique_ptr<detail::minimizer_hashmap_fragment_index_table> table_;

    /// Release the current flat hash table.
    void clear_table();
    /// Reserve a table for the requested bucket count.
    void reserve_table(std::size_t bucket_count);
    /// Reserve from a distinct-hash upper bound without a rehash overlap.
    void reserve_table_for_distinct_upper_bound(std::size_t distinct_count);
    /// Insert one packed offset and count for a minimizer hash.
    void emplace_table(std::uint64_t hash, std::uint64_t packed);

    /// Sort minimizers by the narrowest exact radix key.
    /// @tparam p_minimizer_type Minimizer representation exposing `kmer()`.
    template <class p_minimizer_type>
    static void sort_minimizers_in_place_(std::vector<p_minimizer_type>& sorted_minimizers) {
        if (sorted_minimizers.empty())
            return;
        // `MinimizerRepr::kmer()` deliberately exposes a uint64_t ABI even
        // when the selected representation carries a much narrower hash.  Do
        // not let that ABI choice force radix_sort through known-zero high
        // bytes: XXL_HIFI uses MinimizerRepr<33, 15> with exactly 30 hash bits,
        // so a uint64_t key performs four all-zero radix levels over the whole
        // target before reaching a significant byte.  Returning the smallest
        // unsigned key type that can represent the public kmer() domain
        // preserves the ordering exactly while removing those scans.  The
        // exported 128-bit representation can report a layout hash field
        // wider than 64 bits while kmer() intentionally remains uint64_t; it
        // therefore stays on the uint64_t radix key.
        using minimizer_layout = typename p_minimizer_type::layout_type;
        using public_kmer = decltype(std::declval<const p_minimizer_type&>().kmer());
        static_assert(minimizer_layout::k_hash_bits > 0u);
        static_assert(std::is_same_v<public_kmer, std::uint64_t>);
        using kmer_radix_key = std::conditional_t<
            (minimizer_layout::k_hash_bits <= 8u),
            std::uint8_t,
            std::conditional_t<(minimizer_layout::k_hash_bits <= 16u),
                               std::uint16_t,
                               std::conditional_t<(minimizer_layout::k_hash_bits <= 32u),
                                                  std::uint32_t,
                                                  std::uint64_t>>>;
        /// Narrows the public hash to its exact radix-sort width.
        struct kmer_key {
            /// Return the hash key used by the radix sorter.
            [[nodiscard, gnu::always_inline]]
            inline kmer_radix_key operator()(const p_minimizer_type& m) const noexcept {
                return static_cast<kmer_radix_key>(m.kmer());
            }
        };
        bioseq::sorting::radix_sort(sorted_minimizers.data(),
                                    sorted_minimizers.data() + sorted_minimizers.size(),
                                    kmer_key{});
    }

    /// Build lookup storage from an already sorted caller-owned vector.
    /// @tparam p_minimizer_type Minimizer representation exposing hash and encoded position.
    template <class p_minimizer_type>
    void build_presorted_in_place_(std::vector<p_minimizer_type>& sorted_minimizers,
                                   sequence_size_t target_length,
                                   bool reserve_full_table_once) {
        target_len = target_length;
        clear_table();
        positions.clear();

        const std::size_t total = sorted_minimizers.size();
        if (total == 0u) {
            return;
        }

        positions.resize(total);
        if (reserve_full_table_once) {
            // The admitted Vulkan route reserves for the worst-case D=N once.
            // A single allocation avoids half-size allocation/rehash overlap.
            reserve_table_for_distinct_upper_bound(total);
        } else {
            // Apply the CPU allocation policy for every non-Vulkan caller.
            reserve_table(total / 2u + 16u);
        }
        std::size_t i = 0;
        while (i < total) {
            const std::uint64_t kmer = sorted_minimizers[i].kmer();
            std::size_t j = i;
            while (j < total && sorted_minimizers[j].kmer() == kmer) {
                positions[j] = sorted_minimizers[j].pos_strand_encoded();
                ++j;
            }
            emplace_table(kmer,
                          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32) |
                              static_cast<std::uint64_t>(static_cast<std::uint32_t>(j - i)));
            i = j;
        }
    }

    /// Instrument sorting and lookup construction for one caller-owned vector.
    /// @tparam p_minimizer_type Minimizer representation accepted by the transient lookup.
    template <class p_minimizer_type>
    void build_sorted_in_place(std::vector<p_minimizer_type>& sorted_minimizers,
                               sequence_size_t target_length,
                               bool reserve_full_table_once = false) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("target_index_build");
        auto _scope = bioseq::instrumentation::region(_desc);
        sort_minimizers_in_place_(sorted_minimizers);
        build_presorted_in_place_(sorted_minimizers, target_length, reserve_full_table_once);
    }

  public:
    /// Global `(position << 1) | strand` values in hash-bucket order.
    std::vector<std::uint64_t> positions; // (last_pos<<1)|strand, full uint64
    /// Full target length represented by this lookup.
    sequence_size_t target_len = 0;

    /// Construct an empty lookup.
    minimizer_hashmap_fragment_index();
    /// Release the transient table and position storage.
    ~minimizer_hashmap_fragment_index();
    /// Copy lookup contents into independent storage.
    minimizer_hashmap_fragment_index(const minimizer_hashmap_fragment_index& other);
    /// Replace lookup contents with a copy.
    minimizer_hashmap_fragment_index& operator=(const minimizer_hashmap_fragment_index& other);
    /// Transfer lookup contents without rebuilding the table.
    minimizer_hashmap_fragment_index(minimizer_hashmap_fragment_index&& other) noexcept;
    /// Replace lookup contents by transfer.
    minimizer_hashmap_fragment_index& operator=(minimizer_hashmap_fragment_index&& other) noexcept;

    /// Magic word stored in the transient index binary header.
    static constexpr std::uint32_t k_encoded_magic = 0xB1000001u;
    /// Revision of the transient index binary layout.
    static constexpr std::uint32_t k_encoded_layout_revision = 2u;
    /// Registry name for per-fragment minimizer indexes.
    static constexpr std::string_view index_name = k_minimizers_per_fragment_index_name;

    /// Fixed little-endian transient index header fields.
    struct encoded_header {
        std::uint32_t magic;
        std::uint32_t layout_revision;
        std::uint64_t target_len; ///< Full 64-bit target length.
        std::uint32_t bucket_count;
        std::uint32_t reserved; ///< Padding written as zero
        std::uint64_t total_positions;
    };
    /// Fixed little-endian hash bucket fields.
    struct encoded_bucket {
        std::uint64_t hash;
        std::uint32_t pos_offset;
        std::uint32_t pos_count;
    };
    /// Encoded width of the fixed header in bytes.
    static constexpr std::size_t k_encoded_header_bytes = sizeof(encoded_header);
    /// Encoded width of each fixed hash bucket in bytes.
    static constexpr std::size_t k_encoded_bucket_bytes = sizeof(encoded_bucket);
    static_assert(k_encoded_header_bytes == 32, "wire header layout pinned");
    static_assert(k_encoded_bucket_bytes == 16, "wire bucket layout pinned");
    static_assert(std::endian::native == std::endian::little,
                  "serialized index requires a little-endian host");
    static_assert(sizeof(std::size_t) >= 8, "serialized index requires 64-bit size_t");

    /// Return the number of distinct hash buckets.
    [[nodiscard]] std::size_t table_size() const noexcept;
    /// Return whether the lookup has no hash buckets.
    [[nodiscard]] bool table_empty() const noexcept;
    /// Return bytes currently retained by the hash table.
    [[nodiscard]] std::uint64_t table_allocation_bytes() const noexcept;
    /// Return the peak hash-table bytes retained during this lifetime.
    [[nodiscard]] std::uint64_t table_peak_allocation_bytes() const noexcept;
    /// Visit every hash bucket through an erased callback.
    void for_each_table_entry_erased(
        void* ctx, void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const;

    /// Visit every hash bucket with a typed callable.
    /// @tparam p_function Callable accepting a hash and its packed offset/count value.
    template <class p_function> void for_each_table_entry(p_function&& emit) const {
        using fn = std::remove_reference_t<p_function>;
        for_each_table_entry_erased(&emit, [](void* ctx, std::uint64_t hash, std::uint64_t packed) {
            (*static_cast<fn*>(ctx))(hash, packed);
        });
    }

    /// Build from a caller-owned immutable minimizer sequence.
    /// @tparam p_minimizer_type Minimizer representation accepted by the transient lookup.
    template <class p_minimizer_type>
    void build(const std::vector<p_minimizer_type>& target_minimizers,
               sequence_size_t target_length) {
        // A call-local copy bounds scratch lifetime to this operation so later
        // GPU stages do not retain the target-sized allocation in worker RSS.
        std::vector<p_minimizer_type> sorted_minimizers{target_minimizers.begin(),
                                                        target_minimizers.end()};
        build_sorted_in_place(sorted_minimizers, target_length);
    }

    /// Sort one caller-owned minimizer source by its canonical hash without
    /// constructing the FOA lookup table or a positions vector.  The direct
    /// mapped Vulkan route uses this same production radix implementation,
    /// then streams the sorted source into its final resident allocation.
    /// @tparam p_minimizer_type Minimizer representation exposing `kmer()`.
    template <class p_minimizer_type>
    static void sort_source_by_hash_in_place(std::vector<p_minimizer_type>& source) {
        sort_minimizers_in_place_(source);
    }

    /// Build a transient lookup index by consuming caller-owned minimizers.
    /// Sorting happens in the caller's allocation and the vector is
    /// deterministically deallocated before this function returns.
    /// @tparam p_minimizer_type Minimizer representation accepted by the transient lookup.
    template <class p_minimizer_type>
    void build_consuming(std::vector<p_minimizer_type>&& target_minimizers,
                         sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers, target_length);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Admitted Vulkan build: reserve one table for worst-case D=N so no FOA
    /// growth allocation can overlap it. The caller must acquire positions +
    /// planned-table admission before entering this method.
    /// @tparam p_minimizer_type Minimizer representation accepted by the transient lookup.
    template <class p_minimizer_type>
    void build_consuming_full_table(std::vector<p_minimizer_type>&& target_minimizers,
                                    sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers,
                              target_length,
                              /*reserve_full_table_once=*/true);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Deallocate lookup storage after the final seed consumer.  This is for
    /// an operation-owned transient index only. Borrowed persisted indexes
    /// must never be passed to the post-seed lifetime coordinator.
    void release_storage() noexcept;

    /// Radix-bin pre-extracted global (kmer, encoded_position) pairs.
    /// `pairs` is mutable and radix-sorted in place in caller-owned scratch without copying.
    void build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
                          sequence_size_t target_length);

    /// Return all encoded positions for one minimizer hash.
    [[nodiscard]] std::span<const std::uint64_t> lookup(std::uint64_t kmer_hash) const;

    /// Add a global base-pair offset to every stored position.
    void add_position_offset(sequence_size_t offset);

    /// Return the full target length represented by the lookup.
    [[nodiscard]] sequence_size_t target_length() const {
        return target_len;
    }

    /// Return the exact serialized byte count.
    [[nodiscard]] std::size_t serialize_binary_size() const;

    /// Serialize into an exact-size caller-owned buffer.
    void serialize_to_binary(char* output) const;

    /// Decode and validate one fixed little-endian transient index image.
    [[nodiscard]] static minimizer_hashmap_fragment_index
    deserialize_from_binary(const char* input, std::size_t input_size);
};
static_assert(minimizer_lookup_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model minimizer_lookup_index");
static_assert(persistable_minimizer_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model persistable_minimizer_index");

/// Encode one sorted-dense control without persisting native struct layout.
[[nodiscard]] constexpr std::array<std::byte, 16> encode_minimizer_serialized_bucket(
    const minimizer_hashmap_fragment_index::encoded_bucket& bucket) noexcept {
    std::array<std::byte, 16> output{};
    std::span<std::byte> bytes{output};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(bytes, 0, bucket.hash);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(bytes, 8, bucket.pos_offset);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(bytes, 12, bucket.pos_count);
    return output;
}

/// Decode one exact sorted-dense control from fixed little-endian bytes.
[[nodiscard]] constexpr std::expected<minimizer_hashmap_fragment_index::encoded_bucket,
                                      minimizer_generation_encoding_error>
decode_minimizer_serialized_bucket(std::span<const std::byte> input) noexcept {
    if (input.size() != 16u) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    return minimizer_hashmap_fragment_index::encoded_bucket{
        .hash = minimizer_generation_wire_detail::read_le<std::uint64_t>(input, 0),
        .pos_offset = minimizer_generation_wire_detail::read_le<std::uint32_t>(input, 8),
        .pos_count = minimizer_generation_wire_detail::read_le<std::uint32_t>(input, 12),
    };
}

} // namespace bioseq::indexing
