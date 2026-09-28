/**
 * @file minimizer_hashmap/grouped_soa/transient_index.cppm
 * @brief Defines an in-memory minimizer lookup and its binary bucket format.
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

/// Table capacity and bytes needed for a bounded number of distinct hashes.
struct minimizer_hashmap_table_allocation_plan {
    /// Distinct-hash byund including the  extra space reserved by the planner.
    std::uint64_t distinct_upper_bound = 0u;
    /// Slot count requested before group rounding.
    std::uint64_t requested_slots = 0u;
    /// Power-of-two group count allocated by the table.
    std::uint64_t group_count = 0u;
    /// Usable slot count after reserving the sentinel slot.
    std::uint64_t capacity = 0u;
    /// Bytes allocated for the planned groups.
    std::uint64_t allocation_bytes = 0u;
    /// True if the capacity and allocation calculations succeeded.
    bool valid = false;
};

/// Plan table capacity and memory for the maximum expected number of distinct hashes.
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

    // An empty sketch needs no table allocation. The memory plan must match that
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

/// In-memory hash lookup that returns target positions for matching minimizers.
struct minimizer_hashmap_fragment_index {
  private:
    std::unique_ptr<detail::minimizer_hashmap_fragment_index_table> table_;

    /// Remove all hash entries, creating the table owner if needed.
    void clear_table();
    /// Reserve space for the requested number of hash buckets.
    void reserve_table(std::size_t bucket_count);
    /// Allocate the  full planned table once. The table must have no existing allocation.
    void reserve_table_for_distinct_upper_bound(std::size_t distinct_count);
    /// Insert one packed offset and count for a minimizer hash.
    void emplace_table(std::uint64_t hash, std::uint64_t packed);

    /// Sort minimizers by hash, using the smallest integer width that holds the hash.
    /// @tparam p_minimizer_type Minimizer representation exposing `kmer()`.
    template <class p_minimizer_type>
    static void sort_minimizers_in_place_(std::span<p_minimizer_type> sorted_minimizers) {
        if (sorted_minimizers.empty())
            return;
        // kmer() returns uint64_t even, for short hashes. Sorting a 30-bit hash as
        // uint32_t skips four radix passes over zero bytes. Choose the width from
        // the layout while keeping the full range returned by kmer(). Layouts
        // wider than 64 bits still use its uint64_t result.
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
        /// Converts the hash to the integer width used by the adix sorter.
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

    /// Sort a  vector through the shared span implementation.
    template <class p_minimizer_type>
    static void sort_minimizers_in_place_(std::vector<p_minimizer_type>& sorted_minimizers) {
        sort_minimizers_in_place_(std::span<p_minimizer_type>{sorted_minimizers});
    }

    /// Build hash buckets and copy positions from minimizers already sorted by hash.
    /// @tparam p_minimizer_type Minimizer representation exposing hash and encoded position.
    template <class p_minimizer_type>
    void build_presorted_(std::span<const p_minimizer_type> sorted_minimizers,
                          sequence_size_t target_length,
                          bool reserve_full_table_once) {
        target_len = target_length;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        try {
            clear_table();
        } catch (const std::bad_alloc&) {
            try {
                bioseq_log_progress<logging_level::warning, logging_target::file>(
                    "[bioseq.transient_mpf_index.failure] site=table_owner_allocation "
                    "total={} target_length={} positions_capacity={} full_table={}",
                    static_cast<unsigned long long>(sorted_minimizers.size()),
                    static_cast<unsigned long long>(target_length),
                    static_cast<unsigned long long>(positions.capacity()),
                    reserve_full_table_once);
            } catch (...) {
                // Diagnostics must not replace the table-owner allocation exception.
            }
            throw;
        }
#else
        clear_table();
#endif
        positions.clear();

        const std::size_t total = sorted_minimizers.size();
        if (total == 0u) {
            return;
        }

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        try {
            positions.resize(total);
        } catch (const std::bad_alloc&) {
            try {
                bioseq_log_progress<logging_level::warning, logging_target::file>(
                    "[bioseq.transient_mpf_index.failure] site=positions_resize total={} "
                    "target_length={} positions_capacity={} requested_bytes={} full_table={}",
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(target_length),
                    static_cast<unsigned long long>(positions.capacity()),
                    static_cast<unsigned long long>(bioseq::common::saturating_mul(
                        static_cast<std::uint64_t>(total),
                        static_cast<std::uint64_t>(sizeof(std::uint64_t)))),
                    reserve_full_table_once);
            } catch (...) {
                // Diagnostics must not replace the position allocation exception.
            }
            throw;
        }
#else
        positions.resize(total);
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        try {
#endif
        if (reserve_full_table_once) {
            // Reserve for every minimizer having a different hash. This keeps a larger
            // replacement table from overlapping the first allocation.
            reserve_table_for_distinct_upper_bound(total);
        } else {
            // Other callers start with a smaller table and allow it to grow.
            reserve_table(total / 2u + 16u);
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        } catch (const std::bad_alloc&) {
            const std::uint64_t planned_table_bytes =
                reserve_full_table_once
                    ? plan_minimizer_hashmap_table_allocation(
                          static_cast<std::uint64_t>(total))
                          .allocation_bytes
                    : 0u;
            try {
                bioseq_log_progress<logging_level::warning, logging_target::file>(
                    "[bioseq.transient_mpf_index.failure] site={} total={} target_length={} "
                    "positions_capacity={} planned_table_bytes={} full_table={}",
                    reserve_full_table_once ? "table_rehash_full" : "table_reserve_incremental",
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(target_length),
                    static_cast<unsigned long long>(positions.capacity()),
                    static_cast<unsigned long long>(planned_table_bytes),
                    reserve_full_table_once);
            } catch (...) {
                // Diagnostics must not replace the hash-table allocation exception.
            }
            throw;
        }
#endif
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

    /// Sort the caller's vector and build the lookup, recording the build time.
    /// @tparam p_minimizer_type Minimizer  representation accepted by the transient lookup
    template <class p_minimizer_type>
    void build_sorted_in_place(std::vector<p_minimizer_type>& sorted_minimizers,
                               sequence_size_t target_length,
                               bool reserve_full_table_once = false) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("target_index_build");
        auto _scope = bioseq::instrumentation::region(_desc);
        sort_minimizers_in_place_(sorted_minimizers);
        build_presorted_(std::span<const p_minimizer_type>{sorted_minimizers},
                         target_length,
                         reserve_full_table_once);
    }

  public:
    /** Selects construction without an allocation, so the caller can reserve memory first. */
    struct defer_table_allocation_t final {};
    static inline constexpr defer_table_allocation_t defer_table_allocation{};

    /// Encoded global end positions, grouped by hash: `(position << 1) | strand`.
    std::vector<std::uint64_t> positions; // Includes the strand bit in the low bit.
    /// Full target length represented by this lookup.
    sequence_size_t target_len = 0;

    /// Construct an empty lookup.
    minimizer_hashmap_fragment_index();
    /// Construct without allocating the private table owner.
    explicit minimizer_hashmap_fragment_index(defer_table_allocation_t) noexcept;
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

    /// Fields of the 32-byte little-endian index header.
    struct encoded_header {
        std::uint32_t magic;
        std::uint32_t layout_revision;
        std::uint64_t target_len; ///< Full 64-bit target length.
        std::uint32_t bucket_count;
        std::uint32_t reserved; ///< Reserved field written as zero.
        std::uint64_t total_positions;
    };
    /// Hash and position range stored in each 16-byte little-endian bucket.
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
    /// Return bytes currently allocated for the hash table's entries.
    [[nodiscard]] std::uint64_t table_allocation_bytes() const noexcept;
    /// Return the memory to reserve before allocating the private table owner.
    [[nodiscard]] static std::uint64_t table_owner_allocation_requirement_bytes();
    /// Return the table owner's allocation, size reported by the alloator.
    [[nodiscard]] std::uint64_t table_owner_allocation_bytes() const noexcept;
    /// Return  the highest hash-table allocation total recorded by this owner.
    [[nodiscard]] std::uint64_t table_peak_allocation_bytes() const noexcept;
    /// Call emit for each hash bucket, passing its hash, packed range and caller context.
    void for_each_table_entry_erased(
        void* ctx, void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const;

    /// Call emit for each hash and its packed position range.
    /// @tparam p_function Callable accepting a hash and its packed offset/count value.
    template <class p_function> void for_each_table_entry(p_function&& emit) const {
        using fn = std::remove_reference_t<p_function>;
        for_each_table_entry_erased(&emit, [](void* ctx, std::uint64_t hash, std::uint64_t packed) {
            (*static_cast<fn*>(ctx))(hash, packed);
        });
    }

    /// Build a lookup from a copy, leaving the caller's minimizers unchanged.
    /// @tparam p_minimizer_type Minimizer representation accepted by the transient lookup.
    template <class p_minimizer_type>
    void build(const std::vector<p_minimizer_type>& target_minimizers,
               sequence_size_t target_length) {
        // Release the sorting copy when the build ends, before  later GPU stages need memory.
        std::vector<p_minimizer_type> sorted_minimizers{target_minimizers.begin(),
                                                        target_minimizers.end()};
        build_sorted_in_place(sorted_minimizers, target_length);
    }

    /// Sort the caller's minimizers by hash. GPU builders can then read this order
    /// directly into their final index allocation.
    /// @tparam p_minimizer_type Minimizer representation exposing `kmer()`.
    template <class p_minimizer_type>
    static void sort_source_by_hash_in_place(std::vector<p_minimizer_type>& source) {
        sort_minimizers_in_place_(source);
    }

    /// Sort a contiguous minimizer range by hash in place
    /// @tparam p_minimizer_type Minimizer representation exposing `kmer()`.
    template <class p_minimizer_type>
    static void sort_source_by_hash_in_place(std::span<p_minimizer_type> source) {
        sort_minimizers_in_place_(source);
    }

    /// Sort and build from the caller's vector, then release its storage on success.
    /// @tparam p_minimizer_type Minimizer representation accepted by the transient lookup.
    template <class p_minimizer_type>
    void build_consuming(std::vector<p_minimizer_type>&& target_minimizers,
                         sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers, target_length);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Build  with one table allocation large enough for every hash to be distinct.
    /// The caler must first reserve memory for the positions and planned table.
    /// The input vector is released after a successful build.
    /// @tparam p_minimizer_type Minimizer representation, accepted by the transient lookup.
    template <class p_minimizer_type>
    void build_consuming_full_table(std::vector<p_minimizer_type>&& target_minimizers,
                                    sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers,
                              target_length,
                              /*reserve_full_table_once=*/true);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Build from minimizers already checked and sorted by hash. Requires a fresh
    /// lookup constructed with defer_table_allocation. The caller keeps ownership
    /// of the source and can release it after the build.
    template <class p_minimizer_type>
    void build_presorted_full_table(std::span<const p_minimizer_type> target_minimizers,
                                    sequence_size_t target_length) {
        if (table_ != nullptr || !positions.empty() || positions.capacity() != 0u) {
            throw std::logic_error{
                "pre-admitted presorted minimizer table requires a fresh deferred owner"};
        }
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("target_index_build");
        auto _scope = bioseq::instrumentation::region(_desc);
        build_presorted_(target_minimizers,
                         target_length,
                         /*reserve_full_table_once=*/true);
    }

    /// Release the table  and positions after the last seed lookup. Only the operation
    /// that owns a temporary index may do this. Borrowed indexes must stay alive
    /// for their owner.
    void release_storage() noexcept;

    /// Build from global (hash, encoded_position) pairs. Sort the caller's range
    /// by hash in place, then copy its positions into the lookup.
    void build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
                          sequence_size_t target_length);

    /// Borrow all encoded positions for a hash. Return an empty span if it is absent.
    [[nodiscard]] std::span<const std::uint64_t> lookup(std::uint64_t kmer_hash) const;

    /// Shift every stored position by a base offset, preserving its strand bit
    void add_position_offset(sequence_size_t offset);

    /// Return the full target length represented by the lookup.
    [[nodiscard]] sequence_size_t target_length() const {
        return target_len;
    }

    /// Return the exact serialized byte count.
    [[nodiscard]] std::size_t serialize_binary_size() const;

    /// Write the index to the caller's buffer, which must  hold serialize_binary_size() bytes.
    void serialize_to_binary(char* output) const;

    /// Read the index and check its header, byte size and buiket position ranges.
    [[nodiscard]] static minimizer_hashmap_fragment_index
    deserialize_from_binary(const char* input, std::size_t input_size);
};
static_assert(minimizer_lookup_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model minimizer_lookup_index");
static_assert(persistable_minimizer_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model persistable_minimizer_index");

/// Encode a hash and its position range as a 16-byte little-endian bucket.
[[nodiscard]] constexpr std::array<std::byte, 16> encode_minimizer_serialized_bucket(
    const minimizer_hashmap_fragment_index::encoded_bucket& bucket) noexcept {
    std::array<std::byte, 16> output{};
    std::span<std::byte> bytes{output};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(bytes, 0, bucket.hash);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(bytes, 8, bucket.pos_offset);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(bytes, 12, bucket.pos_count);
    return output;
}

/// Decode a 16-byte little-endian bucket. Reject any other input length.
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
