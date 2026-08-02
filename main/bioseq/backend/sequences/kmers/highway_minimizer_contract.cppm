/**
 * @file highway_minimizer_contract.cppm
 * @brief Target-neutral ABI between the scheduler and generated minimizers.
 *
 * No Highway type is reachable from this module.  Generated fixed-target
 * modules consume these records, while neutral scheduler code owns their
 * storage and lifetime.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.highway_contract;

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.storage.size;

export namespace bioseq::minimizers {

/** Exact target-neutral owner of the existing generic minimizer wire model. */
template<std::size_t MaxSeqLog2, std::size_t KmerLen>
struct MinimizerLayout {
    static constexpr unsigned strand_bits = 1u;
    static constexpr unsigned pos_bits = MaxSeqLog2;
    static constexpr unsigned min_hash_bits =
        (2u * KmerLen < 56u) ? static_cast<unsigned>(2u * KmerLen) : 56u;
    static constexpr int val16_hash = 16 - 1 - static_cast<int>(pos_bits);
    static constexpr int val32_hash = 32 - 1 - static_cast<int>(pos_bits);
    static constexpr int val64_hash = 64 - 1 - static_cast<int>(pos_bits);
    static constexpr int val128_hash =
        128 - 1 - static_cast<int>(pos_bits) - 8;
    static constexpr unsigned storage_bits =
        (val16_hash >= static_cast<int>(min_hash_bits)) ? 16u
        : (val32_hash >= static_cast<int>(min_hash_bits)) ? 32u
        : (val64_hash >= static_cast<int>(min_hash_bits)) ? 64u
        : 128u;
    static constexpr unsigned hash_bits =
        (storage_bits == 16u) ? static_cast<unsigned>(val16_hash)
        : (storage_bits == 32u) ? static_cast<unsigned>(val32_hash)
        : (storage_bits == 64u) ? static_cast<unsigned>(val64_hash)
        : static_cast<unsigned>(val128_hash);
    static constexpr unsigned span_bits = storage_bits == 128u ? 8u : 0u;
    using storage_t =
        std::conditional_t<storage_bits == 16u, std::uint16_t,
        std::conditional_t<storage_bits == 32u, std::uint32_t,
        std::conditional_t<storage_bits == 64u, std::uint64_t, void>>>;
};

template<std::size_t MaxSeqLog2, std::size_t KmerLen,
         bool Is128 =
             (MinimizerLayout<MaxSeqLog2, KmerLen>::storage_bits == 128u)>
struct MinimizerRepr;

template<std::size_t MaxSeqLog2, std::size_t KmerLen>
struct MinimizerRepr<MaxSeqLog2, KmerLen, false> {
    using layout_t = MinimizerLayout<MaxSeqLog2, KmerLen>;
    using storage_t = typename layout_t::storage_t;
    static_assert(!std::is_void_v<storage_t>);

    static constexpr unsigned strand_shift = 0u;
    static constexpr unsigned pos_shift = layout_t::strand_bits;
    static constexpr unsigned hash_shift =
        layout_t::strand_bits + layout_t::pos_bits;
    static constexpr storage_t hash_field_mask =
        (layout_t::hash_bits >= sizeof(storage_t) * 8u)
            ? static_cast<storage_t>(~storage_t{0})
            : static_cast<storage_t>(
                (storage_t{1} << layout_t::hash_bits) - storage_t{1});
    static constexpr storage_t pos_field_mask =
        (layout_t::pos_bits >= sizeof(storage_t) * 8u)
            ? static_cast<storage_t>(~storage_t{0})
            : static_cast<storage_t>(
                (storage_t{1} << layout_t::pos_bits) - storage_t{1});
    static constexpr storage_t strand_field_mask = storage_t{1};
    static constexpr storage_t hash_mask =
        static_cast<storage_t>(hash_field_mask << hash_shift);
    static constexpr storage_t pos_strand_mask =
        static_cast<storage_t>(~hash_mask);

    storage_t value_;

    constexpr MinimizerRepr() noexcept : value_(0) {}
    constexpr MinimizerRepr(
        const std::uint64_t kmer,
        const std::uint64_t last_pos,
        const std::uint64_t strand) noexcept
        : value_(static_cast<storage_t>(
            ((static_cast<storage_t>(kmer) & hash_field_mask) << hash_shift)
            | ((static_cast<storage_t>(last_pos) & pos_field_mask) << pos_shift)
            | (static_cast<storage_t>(strand) & strand_field_mask)))
    {}

    [[nodiscard]] static constexpr MinimizerRepr from_raw_storage(
        const storage_t value) noexcept
    {
        MinimizerRepr result;
        result.value_ = value;
        return result;
    }
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return static_cast<std::uint64_t>(value_) >> hash_shift;
    }
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (static_cast<std::uint64_t>(value_) >> pos_shift)
            & static_cast<std::uint64_t>(pos_field_mask);
    }
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return static_cast<std::uint64_t>(value_) & 1ULL;
    }
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return static_cast<std::uint64_t>(value_)
            & static_cast<std::uint64_t>(pos_strand_mask);
    }
    [[nodiscard]] constexpr storage_t raw_value() const noexcept {
        return value_;
    }
    [[nodiscard]] static constexpr MinimizerRepr getMax() noexcept {
        return from_raw_storage(hash_mask);
    }
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (value_ & hash_mask) == hash_mask;
    }
    constexpr void set_max() noexcept {
        value_ = hash_mask;
    }
    [[nodiscard]] constexpr auto operator<=> (
        const MinimizerRepr& other) const noexcept
    {
        return (value_ & hash_mask) <=> (other.value_ & hash_mask);
    }
    [[nodiscard]] constexpr bool operator==(
        const MinimizerRepr& other) const noexcept
    {
        return (value_ & hash_mask) == (other.value_ & hash_mask);
    }
    [[nodiscard]] static constexpr bool does_sketch_match(
        const MinimizerRepr& first,
        const MinimizerRepr& second) noexcept
    {
        return ((first.value_ ^ second.value_) & hash_mask) == 0u
            && first.value_ != second.value_;
    }
};

template<std::size_t MaxSeqLog2, std::size_t KmerLen>
struct MinimizerRepr<MaxSeqLog2, KmerLen, true> {
    using layout_t = MinimizerLayout<MaxSeqLog2, KmerLen>;
    static_assert(layout_t::pos_bits + 1u <= 64u);

    static constexpr unsigned span_shift_high = 0u;
    static constexpr unsigned hash_shift_high = 8u;
    static constexpr std::uint64_t hash_field_mask =
        layout_t::hash_bits >= 64u
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << layout_t::hash_bits) - std::uint64_t{1};
    static constexpr std::uint64_t pos_field_mask =
        layout_t::pos_bits >= 64u
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << layout_t::pos_bits) - std::uint64_t{1};
    static constexpr std::uint64_t span_field_mask = std::uint64_t{0xFFu};
    static constexpr std::uint64_t high_hash_mask =
        hash_field_mask << hash_shift_high;
    static constexpr std::uint64_t low_pos_mask = pos_field_mask << 1u;

    std::uint64_t low_;
    std::uint64_t high_;

    constexpr MinimizerRepr() noexcept : low_(0), high_(0) {}
    constexpr MinimizerRepr(
        const std::uint64_t kmer,
        const std::uint64_t last_pos,
        const std::uint64_t strand,
        const std::uint64_t span = static_cast<std::uint64_t>(KmerLen)) noexcept
        : low_(((last_pos & pos_field_mask) << 1u) | (strand & 1ULL)),
          high_(((kmer & hash_field_mask) << hash_shift_high)
                | (span & span_field_mask))
    {}

    [[nodiscard]] constexpr std::uint64_t data_low_64() const noexcept {
        return low_;
    }
    [[nodiscard]] constexpr std::uint64_t data_high_64() const noexcept {
        return high_;
    }
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return high_ >> hash_shift_high;
    }
    [[nodiscard]] constexpr std::uint64_t span() const noexcept {
        return high_ & span_field_mask;
    }
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (low_ >> 1u) & pos_field_mask;
    }
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return low_ & 1ULL;
    }
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return low_;
    }
    [[nodiscard]] static constexpr MinimizerRepr getMax() noexcept {
        MinimizerRepr result;
        result.high_ = high_hash_mask;
        return result;
    }
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (high_ & high_hash_mask) == high_hash_mask;
    }
    constexpr void set_max() noexcept {
        high_ = high_hash_mask;
        low_ = 0u;
    }
    [[nodiscard]] constexpr auto operator<=> (
        const MinimizerRepr& other) const noexcept
    {
        return (high_ & high_hash_mask) <=> (other.high_ & high_hash_mask);
    }
    [[nodiscard]] constexpr bool operator==(
        const MinimizerRepr& other) const noexcept
    {
        return (high_ & high_hash_mask) == (other.high_ & high_hash_mask);
    }
    [[nodiscard]] static constexpr bool does_sketch_match(
        const MinimizerRepr& first,
        const MinimizerRepr& second) noexcept
    {
        constexpr std::uint64_t identity_high_mask = ~span_field_mask;
        return ((first.high_ ^ second.high_) & high_hash_mask) == 0u
            && ((first.high_ & identity_high_mask)
                    != (second.high_ & identity_high_mask)
                || first.low_ != second.low_);
    }
};

using MinimapSketchData = MinimizerRepr<28u, 15u>;

static_assert(std::same_as<MinimapSketchData, MinimizerRepr<28u, 15u>>);
static_assert(sizeof(MinimapSketchData) == sizeof(std::uint64_t));
static_assert(alignof(MinimapSketchData) == alignof(std::uint64_t));
static_assert(std::is_standard_layout_v<MinimapSketchData>);
static_assert(std::is_trivially_copyable_v<MinimapSketchData>);

using std::size_t;
using std::uint64_t;

/// Maximum sequence length whose zero-based minimizer positions fit in a
/// representation.  A length of exactly `2^pos_bits` is valid because its
/// final position is `2^pos_bits - 1`; the next base would be truncated by
/// `MinimizerRepr`'s constructor mask.
template <class MinimizerT>
inline constexpr bioseq::storage::sequence_size_t
minimizer_max_sequence_length_v = []() constexpr {
    constexpr auto mask = static_cast<bioseq::storage::sequence_size_t>(
        MinimizerT::pos_field_mask);
    if constexpr (mask
            == (std::numeric_limits<
                bioseq::storage::sequence_size_t>::max)()) {
        return mask;
    } else {
        return static_cast<bioseq::storage::sequence_size_t>(mask + 1u);
    }
}();

template <class MinimizerT>
[[nodiscard]] inline constexpr bool minimizer_query_length_is_representable(
        bioseq::storage::sequence_size_t query_length) noexcept {
    return query_length <= minimizer_max_sequence_length_v<MinimizerT>;
}

/// Fail before any minimizer constructor can mask an out-of-range query
/// position.  Short queries, including `query_length < k`, deliberately pass;
/// sketch kernels return the valid empty minimizer set for that shape.
template <class MinimizerT>
inline void require_representable_minimizer_query_length(
        bioseq::storage::sequence_size_t query_length) {
    if (!minimizer_query_length_is_representable<MinimizerT>(query_length)) {
        throw std::length_error(
            "query length exceeds the selected minimizer position field");
    }
}

static_assert(
    minimizer_max_sequence_length_v<MinimapSketchData>
        == (bioseq::storage::sequence_size_t{1u} << 28u),
    "MinimapSketchData must accept 2^28 bases and reject 2^28 + 1");
static_assert(minimizer_query_length_is_representable<MinimapSketchData>(
    bioseq::storage::sequence_size_t{1u} << 28u));
static_assert(!minimizer_query_length_is_representable<MinimapSketchData>(
    (bioseq::storage::sequence_size_t{1u} << 28u) + 1u));

static_assert(sizeof(MinimapSketchData) == 8,
              "Canonical MinimapSketchData must remain a single uint64_t");
static_assert(alignof(MinimapSketchData) == 8);
static_assert(std::is_standard_layout_v<MinimapSketchData>);
static_assert(std::is_trivially_copyable_v<MinimapSketchData>);
static_assert(std::is_trivially_destructible_v<MinimapSketchData>);

// DOC0256 §4.7 — guard the canonical 64-bit storage invariant.  The postgres
// sidecar / dispatch_buffer paths bake in `sizeof(MinimapSketchData) == 8` and
// the (hash:35 | pos:28 | strand:1) wire layout; if a future change to the
// MinimizerLayout formula promotes (28, 15) to a 128-bit storage band we want
// the build to fail loudly here rather than silently corrupting the on-disk
// blob.
static_assert(MinimizerLayout<28u, 15u>::storage_bits == 64u,
    "MinimapSketchData = MinimizerRepr<28, 15> must remain a 64-bit storage type. "
    "Postgres sidecar / dispatch_buffer rely on sizeof(MinimapSketchData) == 8 "
    "and the bit-layout (hash:35 | pos:28 | strand:1) is hardcoded in the wire "
    "protocol.  Update the sidecar wire format before relaxing this constraint.");

// Sanity check: the canonical layout matches the prior hand-rolled
// constants exactly.  Future MSL/KL choices route through MinimizerRepr.
static_assert(MinimapSketchData::layout_t::pos_bits == 28u);
static_assert(MinimapSketchData::layout_t::hash_bits == 35u);
static_assert(MinimapSketchData::layout_t::strand_bits == 1u);
static_assert(MinimapSketchData::hash_shift == 29u);
static_assert(MinimapSketchData::hash_mask == 0xFFFFFFFFE0000000ULL);

// Legacy alias kept for transition: `MinimapSketch` was the wrapper
// type before the compact 8-byte rewrite.  Now a transparent typedef
// that downstream code uses interchangeably with MinimapSketchData.
using MinimapSketch = MinimapSketchData;

// Templated sketch collector — parameterised on the minimizer
// representation type.  The bench-side dispatch picks a `MinimizerRepr`
// instantiation matching the sequence size bucket; the postgres side
// uses the canonical `MinimapSketchData` alias.
//
// The default template arg keeps every existing call site
// (`sketch_collector collector;`) compiling unchanged — the alias
// `sketch_collector` below is `sketch_collector_t<MinimapSketchData>`.
template<class MinimizerT = MinimapSketchData>
struct sketch_collector_t {
private:
    std::vector<MinimizerT> sketches;
public:
    using minimizer_type = MinimizerT;

    // Earlier we reserved 10 000 entries up front (~160 KB of system-heap
    // storage per collector). Two collectors are built per pair × 400 pairs
    // = ~128 MB of allocator traffic per bench section, of which only ~5 %
    // is actually filled (typical sketches are 50-200 entries). Drop the
    // upfront reserve; std::vector's geometric growth handles the few
    // realloc steps cheaply at small N. If callers know the expected count
    // they can call `.reserve()` themselves.
    sketch_collector_t() : sketches() {};

    /// Pre-reserve capacity to avoid the geometric realloc chain on
    /// long sequences. Used by `minimap_port` when it knows the sequence
    /// length up front: expected minimizer count is roughly `N/w` where
    /// `N` is the sequence length and `w` is the window size; we add a
    /// small safety margin to avoid edge-of-window reallocs. For a 1 Mbp
    /// pair this side-steps ~17 doubling reallocations of the underlying
    /// buffer (each one a memcpy of the previous contents).
    void reserve(size_t expected_count) {
        sketches.reserve(expected_count);
    }

    [[always_inline]] constexpr void collect(MinimizerT&& sketch) {
        sketches.push_back(std::move(sketch));
    }

    [[always_inline]] constexpr void collect(const MinimizerT& sketch) {
        sketches.push_back(sketch);
    }

    /// Append a stable SIMD-compressed batch without a scalar per-record
    /// construction loop. The chronological minimizer kernel writes the raw
    /// representation selected by `MinimizerRepr` directly, then commits the
    /// compacted prefix in one bulk copy. `MinimizerRepr` is deliberately a
    /// trivially-copyable, storage-sized wire value, so the byte copy starts
    /// the same object representation that `from_raw_storage` would produce.
    template<class M = MinimizerT>
    [[nodiscard, always_inline]] bool append_raw_storage(
        const typename M::storage_t* raw,
        std::size_t count)
        requires (
            std::same_as<M, MinimizerT>
            && requires { typename M::storage_t; }
            && std::is_trivially_copyable_v<M>
            && sizeof(M) == sizeof(typename M::storage_t))
    {
        const std::size_t old_size = sketches.size();
        if (count > sketches.max_size() - old_size) {
            return false;
        }
        sketches.resize(old_size + count);
        if (count != 0) {
            std::memcpy(
                static_cast<void*>(sketches.data() + old_size),
                static_cast<const void*>(raw),
                count * sizeof(typename M::storage_t));
        }
        return true;
    }

    constexpr std::vector<MinimizerT> extract_sketches() const {
        return sketches;
    }

    // With MinimapSketch = MinimapSketchData (compact 8-byte struct), the
    // legacy "extract a separate `data[]` vector" pass is now a no-op
    // identity copy. Callers either move the storage out via the rvalue
    // overload (zero-copy) or get a plain copy of the same type.
    constexpr const std::vector<MinimizerT>& extract_sketches_data() const & {
        return sketches;
    }

    constexpr std::vector<MinimizerT> extract_sketches_data() && {
        return std::move(sketches);
    }

    constexpr auto extract_pairs() const {
        auto pairs = sketches
               | std::ranges::views::transform([](const MinimizerT& el) -> std::pair<size_t, uint64_t> {
                     return std::make_pair(static_cast<size_t>(el.last_pos()), el.kmer());
                 })
               | std::ranges::to<std::vector>();
        std::ranges::sort(pairs);
        auto n = std::unique(pairs.begin(), pairs.end()) - pairs.begin();
        pairs.resize(n);
        return pairs;
    }

    [[always_inline]] constexpr void clear() {
        sketches.clear();
    }
};

// Default alias — matches the postgres on-disk + cached-in-memory
// MinimapSketchData layout (28-bit pos, 35-bit hash, 1-bit strand).
using sketch_collector = sketch_collector_t<MinimapSketchData>;

/**
 * Sketch collector whose vector capacity is owned by a caller-supplied memory
 * admission permit.  `Permit` is intentionally a template parameter: the core
 * k-mer module stays independent of the backend memory-admission module while
 * PostgreSQL and tests can supply `MemoryPermit`.
 *
 * The permit is declared before the vector, hence the vector is destroyed
 * first.  Each growth reserves the old retained capacity plus headroom for the
 * replacement allocation before calling `std::vector::reserve()`, then
 * reconciles the permit to the allocator-reported retained capacity.  A denied
 * growth is sticky and emits no partial record; callers must check `ok()` after
 * kernels whose scalar `collect()` surface cannot return a status.
 */
template<class MinimizerT, class Permit>
class admitted_sketch_collector_t {
public:
    using minimizer_type = MinimizerT;
    static constexpr std::size_t initial_growth_records = 8u;

    explicit admitted_sketch_collector_t(
            Permit permit,
            std::uint64_t allocation_headroom_factor)
        : permit_(std::move(permit)),
          allocation_headroom_factor_(allocation_headroom_factor) {
        if (!permit_ || permit_.bytes() != 0u
            || allocation_headroom_factor_ == 0u) {
            failed_ = true;
        }
    }

    admitted_sketch_collector_t(const admitted_sketch_collector_t&) = delete;
    admitted_sketch_collector_t& operator=(
        const admitted_sketch_collector_t&) = delete;
    admitted_sketch_collector_t(admitted_sketch_collector_t&&) = delete;
    admitted_sketch_collector_t& operator=(
        admitted_sketch_collector_t&&) = delete;

    [[nodiscard]] bool reserve(std::size_t expected_count) {
        return ensure_capacity(expected_count);
    }

    [[always_inline]] void collect(MinimizerT&& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(std::move(sketch));
        }
    }

    [[always_inline]] void collect(const MinimizerT& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(sketch);
        }
    }

    template<class M = MinimizerT>
    [[nodiscard, always_inline]] bool append_raw_storage(
        const typename M::storage_t* raw,
        std::size_t count)
        requires (
            std::same_as<M, MinimizerT>
            && requires { typename M::storage_t; }
            && std::is_trivially_copyable_v<M>
            && sizeof(M) == sizeof(typename M::storage_t))
    {
        const std::size_t old_size = sketches_.size();
        if (count > sketches_.max_size() - old_size
            || !ensure_capacity(old_size + count)) {
            failed_ = true;
            return false;
        }
        sketches_.resize(old_size + count);
        if (count != 0u) {
            std::memcpy(
                static_cast<void*>(sketches_.data() + old_size),
                static_cast<const void*>(raw),
                count * sizeof(typename M::storage_t));
        }
        return true;
    }

    [[nodiscard]] const std::vector<MinimizerT>&
    extract_sketches_data() const & noexcept {
        return sketches_;
    }

    [[nodiscard]] bool ok() const noexcept {
        return !failed_;
    }

    [[nodiscard]] std::uint64_t admitted_capacity_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::pair<std::vector<MinimizerT>, Permit>
    release_owned_storage() && {
        return {
            std::move(sketches_),
            std::move(permit_),
        };
    }

private:
    [[nodiscard]] static bool checked_bytes(
            std::size_t count,
            std::uint64_t& result) noexcept {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        if (count > maximum / sizeof(MinimizerT)) return false;
        result = static_cast<std::uint64_t>(count) * sizeof(MinimizerT);
        return true;
    }

    [[nodiscard]] bool ensure_capacity(std::size_t required) {
        if (failed_) return false;
        if (required <= sketches_.capacity()) return true;
        if (required > sketches_.max_size()) {
            failed_ = true;
            return false;
        }

        const std::size_t old_capacity = sketches_.capacity();
        std::size_t requested_capacity = initial_growth_records;
        if (old_capacity != 0u) {
            requested_capacity = old_capacity <= sketches_.max_size() / 2u
                ? old_capacity * 2u
                : required;
        }
        requested_capacity = std::max(requested_capacity, required);

        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t old_bytes = 0u;
        std::uint64_t requested_bytes = 0u;
        if (!checked_bytes(old_capacity, old_bytes)
            || !checked_bytes(requested_capacity, requested_bytes)
            || requested_bytes > maximum / allocation_headroom_factor_
            || permit_.bytes() != old_bytes) {
            failed_ = true;
            return false;
        }
        const std::uint64_t allocation_headroom =
            requested_bytes * allocation_headroom_factor_;
        if (allocation_headroom > maximum - old_bytes
            || !permit_.grow_to(old_bytes + allocation_headroom)) {
            failed_ = true;
            return false;
        }

        try {
            sketches_.reserve(requested_capacity);
        } catch (...) {
            permit_.shrink_to(old_bytes);
            throw;
        }

        std::uint64_t retained_bytes = 0u;
        if (!checked_bytes(sketches_.capacity(), retained_bytes)
            || retained_bytes > allocation_headroom) {
            // The allocation already exists.  Retain all reserved headroom
            // until destruction and fail closed instead of emitting into an
            // allocation whose exact ownership could not be reconciled.
            failed_ = true;
            return false;
        }
        permit_.shrink_to(retained_bytes);
        return true;
    }

    Permit permit_;
    std::vector<MinimizerT> sketches_{};
    std::uint64_t allocation_headroom_factor_ = 0u;
    bool failed_ = false;
};

inline constexpr std::size_t two_stack_max_window = 256u;
inline constexpr std::size_t two_stack_max_kmer = 32u;
inline constexpr std::size_t two_stack_exact_max_kmer = 31u;

/** Target-neutral admission contract for native-u64 ExactSimd records. */
template<class MinimizerT>
concept exact_simd_minimizer_representation =
    std::is_trivially_copyable_v<MinimizerT>
    && sizeof(MinimizerT) == sizeof(std::uint64_t)
    && requires (MinimizerT value, std::uint64_t raw) {
        typename MinimizerT::storage_t;
        requires std::same_as<
            typename MinimizerT::storage_t, std::uint64_t>;
        { MinimizerT::getMax() } -> std::same_as<MinimizerT>;
        { MinimizerT::from_raw_storage(raw) } -> std::same_as<MinimizerT>;
        { value.kmer() } -> std::convertible_to<std::uint64_t>;
        { value.raw_value() } -> std::convertible_to<std::uint64_t>;
        { MinimizerT::hash_mask } -> std::convertible_to<std::uint64_t>;
        { MinimizerT::hash_field_mask } -> std::convertible_to<std::uint64_t>;
        { MinimizerT::pos_field_mask } -> std::convertible_to<std::uint64_t>;
        { MinimizerT::hash_shift } -> std::convertible_to<unsigned>;
        { MinimizerT::pos_shift } -> std::convertible_to<unsigned>;
    };

enum class MinimizerSketchBackend : std::uint8_t {
    Scalar = 0,
    VulkanSegment = 1,
    ExactSimd = 2,
    PaperNtHashSimd = 3,
};

[[nodiscard]] constexpr const char* minimizer_sketch_backend_name(
    const MinimizerSketchBackend backend) noexcept
{
    switch (backend) {
        case MinimizerSketchBackend::Scalar: return "scalar";
        case MinimizerSketchBackend::VulkanSegment: return "vulkan_segment";
        case MinimizerSketchBackend::ExactSimd: return "exact_simd";
        case MinimizerSketchBackend::PaperNtHashSimd:
            return "paper_nthash_simd";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool minimizer_sketch_backend_is_vulkan(
    const MinimizerSketchBackend backend) noexcept
{
    return backend == MinimizerSketchBackend::VulkanSegment;
}

using minimizer_sorted_pair_t =
    std::pair<std::uint64_t, std::uint64_t>;

// Exact status and proof models moved from the static two-stack module.  The
// target-generated wrappers return these values without translating or
// collapsing algorithm evidence.
enum class TwoStackContinuationStatus : std::uint8_t {
    ok = 0,
    invalid_window_size,
    invalid_kmer_size,
    invalid_static_configuration,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    not_initialized,
    already_finalized,
    invalid_state,
    invalid_range,
    non_contiguous_range,
    arithmetic_overflow,
    collector_overflow,
};

/** Operation-originated ExactSimd kernel identity. */
enum class ExactSimdKernelKind : std::uint8_t {
    generic_u64 = 0,
    k15_w10_b8_u32_hash,
};

[[nodiscard]] constexpr std::string_view two_stack_status_name(
    TwoStackContinuationStatus status) noexcept
{
    switch (status) {
        case TwoStackContinuationStatus::ok: return "ok";
        case TwoStackContinuationStatus::invalid_window_size:
            return "invalid_window_size";
        case TwoStackContinuationStatus::invalid_kmer_size:
            return "invalid_kmer_size";
        case TwoStackContinuationStatus::invalid_static_configuration:
            return "invalid_static_configuration";
        case TwoStackContinuationStatus::unsupported_alphabet:
            return "unsupported_alphabet";
        case TwoStackContinuationStatus::unsupported_storage:
            return "unsupported_storage";
        case TwoStackContinuationStatus::unsupported_representation:
            return "unsupported_representation";
        case TwoStackContinuationStatus::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case TwoStackContinuationStatus::not_initialized:
            return "not_initialized";
        case TwoStackContinuationStatus::already_finalized:
            return "already_finalized";
        case TwoStackContinuationStatus::invalid_state:
            return "invalid_state";
        case TwoStackContinuationStatus::invalid_range:
            return "invalid_range";
        case TwoStackContinuationStatus::non_contiguous_range:
            return "non_contiguous_range";
        case TwoStackContinuationStatus::arithmetic_overflow:
            return "arithmetic_overflow";
        case TwoStackContinuationStatus::collector_overflow:
            return "collector_overflow";
    }
    return "unknown";
}

/** Fail-closed error retained by exact compatibility facades. */
class ExactSimdRuntimeError final : public std::runtime_error {
public:
    ExactSimdRuntimeError(
        TwoStackContinuationStatus status,
        std::string_view operation)
        : std::runtime_error(
              std::string{operation}
              + ": ExactSimd minimizer execution rejected: "
              + std::string{two_stack_status_name(status)}),
          status_(status) {}

    [[nodiscard]] TwoStackContinuationStatus status() const noexcept {
        return status_;
    }

private:
    TwoStackContinuationStatus status_;
};

struct TwoStackExecutionProof {
    std::uint64_t bp_consumed = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t vector_batches = 0;
    std::uint64_t vector_lanes_visited = 0;
    std::uint64_t masked_tail_lanes = 0;
    std::uint64_t bridge_symbols_staged = 0;
    std::uint64_t bridge_vector_runs = 0;
    std::uint64_t consecutive_kmer_batches = 0;
    std::uint64_t canonical_scramble_batches = 0;
    std::uint64_t window_reduction_batches = 0;
    std::uint64_t event_generation_batches = 0;
    std::uint64_t stable_compress_batches = 0;
    std::uint64_t direct_event_compress_batches = 0;
    std::uint64_t tie_matrix_event_batches = 0;
    std::uint64_t row_transpose_batches = 0;
    std::uint64_t row_vector_stores = 0;
    std::uint64_t palindrome_lanes = 0;
    std::uint64_t first_window_tie_events = 0;
    std::uint64_t expiry_tie_events = 0;
    std::uint64_t tie_hash_prefilter_batches = 0;
    std::uint64_t tie_hash_prefilter_rejected_lanes = 0;
    std::uint64_t tie_raw_verification_batches = 0;
    std::uint64_t entering_min_transitions = 0;
    std::uint64_t expiry_rescan_transitions = 0;
    std::uint64_t final_flush_records = 0;
    std::uint64_t fragment_symbols_staged = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lanes = 0;
    std::uint64_t packed_word_boundary_merges = 0;
    std::uint64_t ambiguous_symbols = 0;
    std::uint64_t ambiguous_reset_batches = 0;
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_backend_branches = 0;
    std::uint64_t scalar_tail_symbols = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_event_records = 0;
    std::uint64_t scalar_sort_records = 0;
    std::uint64_t scalar_dedup_records = 0;
    // Full, contiguous dna4/u32 inputs may use the original report-76
    // vec_stream collector.  These counters are written by the wrapper that
    // actually invokes that collector; they are deliberately separate from
    // the chronological continuation counters below.  In particular, a
    // resident call is never relabelled as a B8 continuation call.
    std::uint64_t resident_vector_stream_kernel_calls = 0;
    std::uint64_t resident_vector_stream_single_calls = 0;
    std::uint64_t resident_vector_stream_dual_calls = 0;
    std::uint64_t resident_vector_stream_nonempty_work_calls = 0;
    std::uint64_t resident_vector_stream_input_bp = 0;
    std::uint64_t resident_vector_stream_minimizers_emitted = 0;
    std::uint64_t generic_u64_kernel_calls = 0;
    std::uint64_t generic_u64_candidate_blocks = 0;
    std::uint64_t generic_u64_lane_visits = 0;
    std::uint64_t k15_w10_b8_u32_hash_kernel_calls = 0;
    std::uint64_t k15_w10_b8_u32_hash_candidate_blocks = 0;
    std::uint64_t k15_w10_b8_u32_hash_lane_visits = 0;
    std::uint64_t k15_w10_b8_packed22_groups = 0;
    std::uint64_t k15_w10_b8_packed22_lane_visits = 0;
    bool saturated = false;

    static void add_counter(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    void combine(const TwoStackExecutionProof& other) noexcept {
        saturated = saturated || other.saturated;
#define BIOSEQ_COMBINE_TWO_STACK_COUNTER(name) \
        add_counter(name, other.name, saturated)
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(bp_consumed);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(minimizers_emitted);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(vector_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(vector_lanes_visited);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(masked_tail_lanes);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(bridge_symbols_staged);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(bridge_vector_runs);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(consecutive_kmer_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(canonical_scramble_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(window_reduction_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(event_generation_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(stable_compress_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(direct_event_compress_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(tie_matrix_event_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(row_transpose_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(row_vector_stores);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(palindrome_lanes);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(first_window_tie_events);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(expiry_tie_events);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(tie_hash_prefilter_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            tie_hash_prefilter_rejected_lanes);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(tie_raw_verification_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(entering_min_transitions);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(expiry_rescan_transitions);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(final_flush_records);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(fragment_symbols_staged);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(packed_decode_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(packed_decode_lanes);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(packed_word_boundary_merges);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(ambiguous_symbols);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(ambiguous_reset_batches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_orchestration_steps);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(fail_closed_rejections);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_backend_branches);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_tail_symbols);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_hash_symbols);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_window_transitions);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_event_records);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_sort_records);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(scalar_dedup_records);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            resident_vector_stream_kernel_calls);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            resident_vector_stream_single_calls);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            resident_vector_stream_dual_calls);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            resident_vector_stream_nonempty_work_calls);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            resident_vector_stream_input_bp);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            resident_vector_stream_minimizers_emitted);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(generic_u64_kernel_calls);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(generic_u64_candidate_blocks);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(generic_u64_lane_visits);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            k15_w10_b8_u32_hash_kernel_calls);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            k15_w10_b8_u32_hash_candidate_blocks);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            k15_w10_b8_u32_hash_lane_visits);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(k15_w10_b8_packed22_groups);
        BIOSEQ_COMBINE_TWO_STACK_COUNTER(
            k15_w10_b8_packed22_lane_visits);
#undef BIOSEQ_COMBINE_TWO_STACK_COUNTER
    }

    [[nodiscard]] constexpr bool accepted(
            bool require_nonempty_vector_work = false) const noexcept {
        const std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        const bool resident_payload_present =
            resident_vector_stream_kernel_calls != 0
            || resident_vector_stream_single_calls != 0
            || resident_vector_stream_dual_calls != 0
            || resident_vector_stream_nonempty_work_calls != 0
            || resident_vector_stream_input_bp != 0
            || resident_vector_stream_minimizers_emitted != 0;
        if (resident_payload_present) {
            const bool stream_call_sum_valid =
                resident_vector_stream_single_calls
                    <= maximum - resident_vector_stream_dual_calls
                && resident_vector_stream_single_calls
                    + resident_vector_stream_dual_calls
                    == resident_vector_stream_kernel_calls;
            const bool continuation_payload_absent =
                vector_batches == 0
                && vector_lanes_visited == 0
                && masked_tail_lanes == 0
                && bridge_symbols_staged == 0
                && bridge_vector_runs == 0
                && consecutive_kmer_batches == 0
                && canonical_scramble_batches == 0
                && window_reduction_batches == 0
                && event_generation_batches == 0
                && stable_compress_batches == 0
                && direct_event_compress_batches == 0
                && tie_matrix_event_batches == 0
                && row_transpose_batches == 0
                && row_vector_stores == 0
                && palindrome_lanes == 0
                && first_window_tie_events == 0
                && expiry_tie_events == 0
                && tie_hash_prefilter_batches == 0
                && tie_hash_prefilter_rejected_lanes == 0
                && tie_raw_verification_batches == 0
                && entering_min_transitions == 0
                && expiry_rescan_transitions == 0
                && final_flush_records == 0
                && fragment_symbols_staged == 0
                && packed_decode_batches == 0
                && packed_decode_lanes == 0
                && packed_word_boundary_merges == 0
                && ambiguous_symbols == 0
                && ambiguous_reset_batches == 0
                && scalar_orchestration_steps == 0
                && generic_u64_kernel_calls == 0
                && generic_u64_candidate_blocks == 0
                && generic_u64_lane_visits == 0
                && k15_w10_b8_u32_hash_kernel_calls == 0
                && k15_w10_b8_u32_hash_candidate_blocks == 0
                && k15_w10_b8_u32_hash_lane_visits == 0
                && k15_w10_b8_packed22_groups == 0
                && k15_w10_b8_packed22_lane_visits == 0;
            const bool resident_backend = !saturated
                && resident_vector_stream_kernel_calls > 0
                && stream_call_sum_valid
                && resident_vector_stream_nonempty_work_calls
                    <= resident_vector_stream_kernel_calls
                && (resident_vector_stream_nonempty_work_calls == 0
                    || resident_vector_stream_minimizers_emitted > 0)
                && resident_vector_stream_input_bp == bp_consumed
                && resident_vector_stream_minimizers_emitted
                    == minimizers_emitted
                && fail_closed_rejections == 0
                && scalar_backend_branches == 0
                && scalar_tail_symbols == 0
                && scalar_hash_symbols == 0
                && scalar_window_transitions == 0
                && scalar_event_records == 0
                && scalar_sort_records == 0
                && scalar_dedup_records == 0
                && continuation_payload_absent;
            return resident_backend
                && (!require_nonempty_vector_work
                    || (bp_consumed > 0
                        && resident_vector_stream_nonempty_work_calls > 0));
        }
        const bool generic_selected = generic_u64_kernel_calls > 0
            && k15_w10_b8_u32_hash_kernel_calls == 0;
        const bool fast_selected = generic_u64_kernel_calls == 0
            && k15_w10_b8_u32_hash_kernel_calls > 0;
        const bool typed_route_complete =
            (generic_selected
                && generic_u64_candidate_blocks == vector_batches
                && generic_u64_lane_visits == vector_lanes_visited
                && k15_w10_b8_u32_hash_candidate_blocks == 0
                && k15_w10_b8_u32_hash_lane_visits == 0
                && k15_w10_b8_packed22_groups == 0
                && k15_w10_b8_packed22_lane_visits == 0)
            || (fast_selected
                && k15_w10_b8_u32_hash_candidate_blocks == vector_batches
                && k15_w10_b8_u32_hash_lane_visits == vector_lanes_visited
                && k15_w10_b8_packed22_groups
                    == k15_w10_b8_u32_hash_candidate_blocks
                && k15_w10_b8_packed22_lane_visits
                    == k15_w10_b8_u32_hash_lane_visits
                && generic_u64_candidate_blocks == 0
                && generic_u64_lane_visits == 0);
        const std::uint64_t route_calls = generic_selected
            ? generic_u64_kernel_calls
            : k15_w10_b8_u32_hash_kernel_calls;
        const bool exact_backend = !saturated
            && fail_closed_rejections == 0
            && bridge_symbols_staged == 0
            && bridge_vector_runs == 0
            && scalar_backend_branches == 0
            && scalar_tail_symbols == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_event_records == 0
            && scalar_sort_records == 0
            && scalar_dedup_records == 0
            && packed_decode_lanes == bp_consumed
            && fragment_symbols_staged == bp_consumed
            && ((bp_consumed == 0 && packed_decode_batches == 0)
                || (bp_consumed > 0 && packed_decode_batches > 0))
            && typed_route_complete;
        if (!exact_backend) return false;

        const bool has_vector_activity = vector_batches != 0
            || vector_lanes_visited != 0
            || generic_u64_candidate_blocks != 0
            || k15_w10_b8_u32_hash_candidate_blocks != 0
            || k15_w10_b8_packed22_groups != 0
            || generic_u64_lane_visits != 0
            || k15_w10_b8_u32_hash_lane_visits != 0
            || k15_w10_b8_packed22_lane_visits != 0;
        if (!has_vector_activity) {
            const bool flush_batches_complete =
                event_generation_batches == route_calls
                && stable_compress_batches == route_calls;
            const bool empty_stage_consistent =
                vector_batches == 0
                && vector_lanes_visited == 0
                && masked_tail_lanes == 0
                && consecutive_kmer_batches == 0
                && canonical_scramble_batches == 0
                && window_reduction_batches == 0
                && flush_batches_complete
                && direct_event_compress_batches == 0
                && tie_matrix_event_batches == 0
                && row_transpose_batches == 0
                && row_vector_stores == 0;
            return empty_stage_consistent
                && !require_nonempty_vector_work;
        }

        const bool event_routes_complete =
            direct_event_compress_batches <= maximum - tie_matrix_event_batches
            && direct_event_compress_batches + tie_matrix_event_batches
                == vector_batches
            && (tie_matrix_event_batches == 0
                || (row_transpose_batches > 0 && row_vector_stores > 0));
        const bool event_batches_complete =
            vector_batches <= maximum - route_calls
            && event_generation_batches == vector_batches + route_calls;
        const bool stable_batches_complete =
            vector_batches <= maximum - route_calls
            && stable_compress_batches >= vector_batches + route_calls;
        const bool active_stage_consistent =
            vector_batches > 0
            && vector_lanes_visited > 0
            && consecutive_kmer_batches == canonical_scramble_batches
            && consecutive_kmer_batches <= vector_batches
            && window_reduction_batches == vector_batches
            && event_batches_complete
            && stable_batches_complete
            && event_routes_complete
            && packed_decode_batches > 0
            && packed_decode_lanes > 0
            && typed_route_complete;
        if (!active_stage_consistent) return false;
        return !require_nonempty_vector_work
            || (bp_consumed > 0
                && consecutive_kmer_batches > 0
                && canonical_scramble_batches > 0);
    }

    [[nodiscard]] constexpr bool operator==(
        const TwoStackExecutionProof&) const noexcept = default;
};

struct TwoStackFinalizeResult final {
    TwoStackContinuationStatus status = TwoStackContinuationStatus::ok;
    TwoStackExecutionProof proof{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == TwoStackContinuationStatus::ok;
    }
};

/** Exact packed paper-ntHash status and evidence models. */
enum class PackedPaperNthashStatus : std::uint8_t {
    ok = 0,
    invalid_configuration,
    input_too_short,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    invalid_range,
    input_too_large,
};

/** Frozen local identity; persisted/index metadata may mirror, never infer it. */
inline constexpr std::uint32_t packed_paper_nthash_scheme_id =
    0x504E5431u; // "PNT1": paper u32, R=7, wrapping fw+rc, endpoint-v1.
inline constexpr std::size_t packed_paper_nthash_max_window = 64;

/**
 * Algorithm-local evidence for the packed paper-ntHash kernel.
 *
 * The scalar append count is truthful orchestration after SIMD has selected a
 * complete (hash, position, strand) event.  It is not a hash/window fallback.
 * The four fallback counters must remain zero on every accepted execution.
 */
struct PackedPaperNthashExecutionProof {
    std::uint32_t scheme_id = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t nthash_rotation = 0;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t segments_combined = 0;
    std::uint64_t logical_streams = 0;
    std::uint64_t physical_vector_lanes = 0;
    std::uint64_t vector_registers_per_step = 0;
    std::uint64_t packed_decode_batches = 0;
    /** SIMD lane visits, including per-lane K/W context; not unique input bp. */
    std::uint64_t packed_decode_lane_visits = 0;
    std::uint64_t hash_initialization_batches = 0;
    std::uint64_t rolling_hash_batches = 0;
    std::uint64_t hash_table_lookup_batches = 0;
    std::uint64_t canonical_sum_batches = 0;
    std::uint64_t window_update_batches = 0;
    std::uint64_t suffix_rebuild_batches = 0;
    std::uint64_t emission_decision_batches = 0;
    std::uint64_t emission_mask_store_batches = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t tied_endpoint_records_emitted = 0;
    std::uint64_t stream_boundary_records_buffered = 0;
    std::uint64_t stream_boundary_records_committed = 0;
    std::uint64_t stream_boundary_duplicates_suppressed = 0;
    std::uint64_t scalar_collector_appends = 0;
    std::uint64_t partial_chunk_callback_batches = 0;
    std::uint64_t masked_tail_lane_visits = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_minimum_decisions = 0;
    std::uint64_t fallback_calls = 0;
    bool positions_are_segment_local = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const PackedPaperNthashExecutionProof&) const noexcept = default;

    static void saturating_add(
        std::uint64_t& destination,
        std::uint64_t increment,
        bool& did_saturate) noexcept
    {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (increment > maximum - destination) {
            destination = maximum;
            did_saturate = true;
        } else {
            destination += increment;
        }
    }

    /** Saturating, semantic-identity-checked segment aggregation. */
    void combine(const PackedPaperNthashExecutionProof& other) noexcept {
        if (other == PackedPaperNthashExecutionProof{}) return;
        if (*this == PackedPaperNthashExecutionProof{}) {
            *this = other;
            return;
        }
        const bool same_identity =
            segments_combined > 0
            && other.segments_combined > 0
            && scheme_id == other.scheme_id
            && kmer_size == other.kmer_size
            && window_size == other.window_size
            && nthash_rotation == other.nthash_rotation
            && source_storage_unit_bytes
                == other.source_storage_unit_bytes
            && logical_streams == other.logical_streams
            && physical_vector_lanes == other.physical_vector_lanes
            && vector_registers_per_step
                == other.vector_registers_per_step
            && positions_are_segment_local
                == other.positions_are_segment_local;
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch || !same_identity;
        saturated = saturated || other.saturated;
#define BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(name) \
        saturating_add(name, other.name, saturated)
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(input_symbols);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(input_storage_units);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(segments_combined);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(packed_decode_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(packed_decode_lane_visits);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(hash_initialization_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(rolling_hash_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(hash_table_lookup_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(canonical_sum_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(window_update_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(suffix_rebuild_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(emission_decision_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(emission_mask_store_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(minimizers_emitted);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(tied_endpoint_records_emitted);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(stream_boundary_records_buffered);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(stream_boundary_records_committed);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(stream_boundary_duplicates_suppressed);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(scalar_collector_appends);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(partial_chunk_callback_batches);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(masked_tail_lane_visits);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(fail_closed_rejections);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(scalar_hash_symbols);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(scalar_window_transitions);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(scalar_minimum_decisions);
        BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD(fallback_calls);
#undef BIOSEQ_PACKED_PAPER_ADD_PROOF_FIELD
    }

    [[nodiscard]] constexpr bool accepted(
        bool require_nonempty_vector_work = true) const noexcept {
        const bool vector_work_ok = !require_nonempty_vector_work
            || (packed_decode_batches > 0
                && packed_decode_lane_visits > 0
                && hash_initialization_batches > 0
                && hash_table_lookup_batches > 0
                && canonical_sum_batches > 0
                && window_update_batches > 0
                && emission_decision_batches > 0
                && emission_mask_store_batches > 0);
        return !saturated
            && !aggregation_mismatch
            && scheme_id == packed_paper_nthash_scheme_id
            && kmer_size > 0
            && window_size > 0
            && window_size <= packed_paper_nthash_max_window
            && nthash_rotation == 7
            && source_storage_unit_bytes >= sizeof(std::uint32_t)
            && logical_streams == 8
            && (physical_vector_lanes == 4
                || physical_vector_lanes == 8)
            && vector_registers_per_step * physical_vector_lanes == 8
            && segments_combined > 0
            && positions_are_segment_local
            && vector_work_ok
            && canonical_sum_batches == window_update_batches
            && minimizers_emitted == scalar_collector_appends
            && stream_boundary_records_buffered
                == stream_boundary_records_committed
                    + stream_boundary_duplicates_suppressed
            && fail_closed_rejections == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_minimum_decisions == 0
            && fallback_calls == 0;
    }
};

struct PackedPaperNthashResult {
    PackedPaperNthashStatus status = PackedPaperNthashStatus::ok;
    PackedPaperNthashExecutionProof proof{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == PackedPaperNthashStatus::ok;
    }

    /** Kernel-local admission only; pipeline routing adds stronger evidence. */
    [[nodiscard]] constexpr bool kernel_proof_accepted(
        bool require_nonempty_vector_work = true) const noexcept {
        return ok() && proof.accepted(require_nonempty_vector_work);
    }
};


/** Codes are translated explicitly from the central scheduler shape registry. */
enum class PackedSequenceAlphabet : std::uint8_t {
    dna4 = 0u,
    dna5 = 1u,
};

/**
 * Borrowed, already-resident packed sequence segment.
 *
 * `storage` points at the first aligned packed unit. `begin_symbol` is the
 * leading-symbol offset within that borrowed unit range and `end_symbol` is
 * the exclusive local packed coordinate, so the biological length is
 * `end_symbol - begin_symbol`. Continuation state owns the global biological
 * coordinate. Packing is least-significant-symbol first, matching the only
 * sequence storage order currently registered by the scheduler.
 */
struct PackedSequenceKernelView final {
    const std::byte* storage = nullptr;
    std::size_t storage_units = 0u;
    std::size_t begin_symbol = 0u;
    std::size_t end_symbol = 0u;
    std::uint8_t alphabet_code = 0u;
    std::uint8_t bits_per_symbol = 0u;
    std::uint8_t storage_unit_bytes = 0u;
    bool final_segment = false;
};

/** Batch-oriented output adapter; callbacks are never invoked per base/lane. */
struct MinimizerSink final {
    void* state = nullptr;
    std::span<MinimapSketchData> (*acquire)(
        void*, std::size_t minimum) = nullptr;
    void (*commit)(void*, std::size_t initialized) noexcept = nullptr;
};

/** One borrowed packed fragment consumed synchronously by an ExactSimd call. */
struct PackedSequenceKernelFragment final {
    const std::byte* storage = nullptr;
    std::size_t storage_units = 0u;
    std::size_t fragment_bp_size = 0u;
    std::size_t fragment_begin_bp = 0u;
    std::size_t consume_begin_bp = 0u;
    std::size_t consume_end_bp = 0u;
};

/**
 * Pull source for a bounded fragment interval. The target-local continuation
 * is created and finalized during the single table call; only borrowed packed
 * fragments cross this neutral callback boundary.
 */
struct PackedSequenceFragmentSource final {
    void* state = nullptr;
    std::size_t first_fragment = 0u;
    std::size_t fragment_end = 0u;
    PackedSequenceKernelFragment (*read)(void*, std::size_t) = nullptr;
    void (*release)(void*, std::size_t) noexcept = nullptr;

    [[nodiscard]] constexpr bool present() const noexcept {
        return read != nullptr || release != nullptr;
    }
};

/**
 * Batch adapter shared by all generated minimizer families. Exact kernels keep
 * their established collector surface; acquire/commit is crossed only when a
 * full local batch is ready or the coarse wrapper finishes.
 */
template<class MinimizerT>
requires std::same_as<MinimizerT, MinimapSketchData>
class minimizer_sink_collector {
public:
    using minimizer_type = MinimizerT;
    static constexpr std::size_t batch_capacity = 256u;

    explicit minimizer_sink_collector(MinimizerSink sink) noexcept
        : sink_(sink),
          failed_(sink_.acquire == nullptr || sink_.commit == nullptr) {}

    void collect(MinimizerT&& sketch) {
        enqueue(std::move(sketch));
    }

    void collect(const MinimizerT& sketch) {
        enqueue(sketch);
    }

    template<class M = MinimizerT>
    [[nodiscard]] bool append_raw_storage(
        const typename M::storage_t* raw,
        std::size_t count)
        requires (
            std::same_as<M, MinimizerT>
            && requires { typename M::storage_t; }
            && std::is_trivially_copyable_v<M>
            && sizeof(M) == sizeof(typename M::storage_t))
    {
        if (!flush() || failed_) {
            return false;
        }
        if (count == 0u) {
            return true;
        }
        auto destination = sink_.acquire(sink_.state, count);
        if (destination.size() < count || destination.data() == nullptr) {
            failed_ = true;
            return false;
        }
        std::memcpy(
            static_cast<void*>(destination.data()),
            static_cast<const void*>(raw),
            count * sizeof(typename M::storage_t));
        sink_.commit(sink_.state, count);
        emitted_ += count;
        return true;
    }

    [[nodiscard]] bool reserve(std::size_t) const noexcept {
        return !failed_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return emitted_ + buffered_;
    }

    /** Records whose sink commit callback has completed. */
    [[nodiscard]] std::size_t committed_size() const noexcept {
        return emitted_;
    }

    [[nodiscard]] bool ok() const noexcept {
        return !failed_;
    }

    [[nodiscard]] bool flush() {
        if (failed_) {
            return false;
        }
        if (buffered_ == 0u) {
            return true;
        }
        auto destination = sink_.acquire(sink_.state, buffered_);
        if (destination.size() < buffered_ || destination.data() == nullptr) {
            failed_ = true;
            return false;
        }
        std::copy_n(buffer_.begin(), buffered_, destination.begin());
        sink_.commit(sink_.state, buffered_);
        emitted_ += buffered_;
        buffered_ = 0u;
        return true;
    }

private:
    template<class Value>
    void enqueue(Value&& sketch) {
        if (failed_) {
            return;
        }
        if (buffered_ == batch_capacity && !flush()) {
            return;
        }
        buffer_[buffered_++] = std::forward<Value>(sketch);
    }

    MinimizerSink sink_{};
    std::array<MinimizerT, batch_capacity> buffer_{};
    std::size_t buffered_ = 0u;
    std::size_t emitted_ = 0u;
    bool failed_ = false;
};

namespace kernel_detail {

/** Header shape consumed by the existing zero-copy compact range adapter. */
struct borrowed_packed_header {
    const void* data = nullptr;
    std::size_t bp_size = 0u;
    std::uint8_t storage_unit_byte_width = 0u;
    std::uint16_t padding_letters = 0u;
};

template<sequence_alphabet Alphabet, class StorageUnit>
struct borrowed_packed_instance {
    using alphabet_type = Alphabet;
    using storage_unit = StorageUnit;
    using compact_sequence_stdc_type = borrowed_packed_header;

    static constexpr std::size_t letter_bit_width =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>);
    static constexpr std::size_t byte_width = sizeof(StorageUnit);
    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/** Validate and borrow a packed view without materialising or re-encoding. */
template<sequence_alphabet Alphabet, class StorageUnit, class Fn>
[[nodiscard]] bool with_borrowed_compact_range(
    const PackedSequenceKernelView& view,
    Fn&& fn)
{
    using instance = borrowed_packed_instance<Alphabet, StorageUnit>;
    using range_type = compact_sequence_range<instance>;
    constexpr std::size_t bits_per_symbol = instance::letter_bit_width;
    constexpr std::size_t letters_per_unit =
        sizeof(StorageUnit) * 8u / bits_per_symbol;

    if (view.bits_per_symbol != bits_per_symbol
        || view.storage_unit_bytes != sizeof(StorageUnit)
        || view.end_symbol < view.begin_symbol
        || (view.end_symbol != 0u && view.storage == nullptr)
        || (view.storage != nullptr
            && reinterpret_cast<std::uintptr_t>(view.storage)
                % alignof(StorageUnit) != 0u)) {
        return false;
    }

    const std::size_t bp_size = view.end_symbol;
    if (bp_size > (std::numeric_limits<std::size_t>::max)()
            - (letters_per_unit - 1u)) {
        return false;
    }
    const std::size_t expected_units =
        (bp_size + letters_per_unit - 1u) / letters_per_unit;
    if (view.storage_units != expected_units) {
        return false;
    }
    const std::size_t padding =
        expected_units * letters_per_unit - bp_size;
    if (padding > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }

    borrowed_packed_header header{
        .data = view.storage,
        .bp_size = bp_size,
        .storage_unit_byte_width =
            static_cast<std::uint8_t>(sizeof(StorageUnit)),
        .padding_letters = static_cast<std::uint16_t>(padding),
    };
    range_type range{header};
    std::forward<Fn>(fn)(range);
    return true;
}

} // namespace kernel_detail

enum class MinimizerKernelStatus : std::uint8_t {
    ok = 0u,
    invalid_sequence_view,
    invalid_configuration,
    invalid_continuation,
    sink_rejected,
};

enum class MinimizerKernelAlgorithm : std::uint8_t {
    none = 0u,
    reference_minimap_int,
    two_stack_nthash,
    paper_nthash,
};

/**
 * Biological ExactSimd sub-route selected by neutral orchestration.
 *
 * This is not a Highway target choice.  The scheduler has already selected
 * one fixed-target table before either algorithm is entered; the target leaf
 * merely selects between the two existing two-stack algorithms for the
 * admitted input shape.
 */
enum class TwoStackRoute : std::uint8_t {
    chronological_continuation = 0u,
    resident_report76 = 1u,
};

struct MinimizerKernelInvocation final {
    PackedSequenceKernelView sequence{};
    PackedSequenceFragmentSource fragments{};
    std::size_t window = 0u;
    std::size_t kmer = 0u;
    std::size_t emitted_position_base = 0u;
    TwoStackRoute two_stack_route =
        TwoStackRoute::chronological_continuation;
    MinimizerSink sink{};
};

struct ReferenceMinimizerExecutionProof final {
    std::uint64_t kernel_calls = 0u;
    std::uint64_t vector_callback_batches = 0u;
    std::uint64_t vector_lane_visits = 0u;
    std::uint64_t last_storage_unit_bytes = 0u;
};

/** Exact family evidence; only the member named by `algorithm` is populated. */
struct MinimizerExecutionProof final {
    MinimizerKernelAlgorithm algorithm = MinimizerKernelAlgorithm::none;
    ReferenceMinimizerExecutionProof reference{};
    TwoStackExecutionProof two_stack{};
    PackedPaperNthashExecutionProof paper{};
    std::int64_t target = 0;
};

struct MinimizerKernelResult final {
    MinimizerExecutionProof proof{};
    std::size_t emitted = 0u;
    MinimizerKernelStatus status = MinimizerKernelStatus::ok;
    /** Unchanged numeric value of the selected family's native status enum. */
    std::uint32_t family_status = 0u;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == MinimizerKernelStatus::ok;
    }
};

using MinimizerKernelFunction = MinimizerKernelResult (*)(
    MinimizerKernelInvocation&);

struct MinimizerKernels final {
    MinimizerKernelFunction reference_minimap_int = nullptr;
    MinimizerKernelFunction two_stack_nthash = nullptr;
    MinimizerKernelFunction paper_nthash = nullptr;
};

static_assert(std::is_standard_layout_v<PackedSequenceKernelView>);
static_assert(std::is_trivially_copyable_v<PackedSequenceKernelView>);
static_assert(std::is_standard_layout_v<MinimizerSink>);
static_assert(std::is_trivially_copyable_v<MinimizerSink>);
static_assert(std::is_standard_layout_v<PackedSequenceKernelFragment>);
static_assert(std::is_trivially_copyable_v<PackedSequenceKernelFragment>);
static_assert(std::is_standard_layout_v<PackedSequenceFragmentSource>);
static_assert(std::is_trivially_copyable_v<PackedSequenceFragmentSource>);
static_assert(std::is_standard_layout_v<MinimizerKernelInvocation>);
static_assert(std::is_trivially_copyable_v<MinimizerKernelInvocation>);
static_assert(std::is_standard_layout_v<ReferenceMinimizerExecutionProof>);
static_assert(std::is_trivially_copyable_v<ReferenceMinimizerExecutionProof>);
static_assert(std::is_standard_layout_v<MinimizerExecutionProof>);
static_assert(std::is_trivially_copyable_v<MinimizerExecutionProof>);
static_assert(std::is_standard_layout_v<MinimizerKernelResult>);
static_assert(std::is_trivially_copyable_v<MinimizerKernelResult>);
static_assert(std::is_standard_layout_v<MinimizerKernels>);
static_assert(std::is_trivially_copyable_v<MinimizerKernels>);

} // namespace bioseq::minimizers
