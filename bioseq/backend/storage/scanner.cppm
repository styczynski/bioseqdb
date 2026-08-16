/**
 * @file
 * @brief Scanner contracts for resident and bounded-memory sequence access.
 *
 * A fragmented scanner owns the backing lifetime of every loaded fragment.
 * Consumers may retain at most the number of fragments allowed by their local
 * cache and must call `release_fragment()` when evicting one. Resident
 * scanners expose the same compile-time contract without adding runtime state.
 */
module;

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

export module bioseq.storage.scanner;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.storage.size;
import bioseq.sequences;

static_assert(__cplusplus >= 202302L);

export namespace bioseq::storage {

/** Default fragment length in packed storage units. */
inline constexpr std::size_t default_fragment_storage_units = 1024u;

/** Terminate after an internal lazy-sequence bounds or lifetime violation. */
[[noreturn]] [[gnu::cold]] inline void abort_lazy_sequence_access(
    std::string_view message)
{
    bioseq_log<logging_level::error>("[lazy_sequence] {}", message);
    std::abort();
}

/** Non-owning view and logical coordinates for one loaded fragment. */
template <class p_range>
struct sequence_fragment {
    using storage_unit =
        typename compact_sequence_range<p_range>::traits::storage_unit;
    std::span<const storage_unit> data;
    sequence_size_t logical_offset = 0u;
    sequence_size_t logical_length = 0u;
    std::size_t fragment_index = (std::numeric_limits<std::size_t>::max)();
    std::uint16_t trailing_padding = 0u;
};

/** Minimum compile-time surface implemented by every sequence scanner. */
template <class p_scanner>
concept sequence_scanner = requires {
    typename p_scanner::storage_unit;
    typename p_scanner::base_container;
    { p_scanner::is_fragmented() } -> std::same_as<bool>;
    { p_scanner::fragment_storage_units }
        -> std::convertible_to<std::size_t>;
};

/** Scanner contract for sources loaded and evicted one fragment at a time. */
template <class p_scanner>
concept fragmented_sequence_scanner = sequence_scanner<p_scanner>
    && p_scanner::is_fragmented()
    && requires(p_scanner scanner, std::size_t fragment_index) {
        { scanner.bp_size() } -> std::convertible_to<sequence_size_t>;
        { scanner.storage_unit_count() }
            -> std::convertible_to<sequence_size_t>;
        { scanner.storage_byte_count() }
            -> std::convertible_to<sequence_size_t>;
        { scanner.fragment_count() } -> std::convertible_to<std::size_t>;
        { scanner.estimated_bp_size() }
            -> std::convertible_to<sequence_size_t>;
        { scanner.load_fragment(fragment_index) }
            -> std::same_as<typename p_scanner::base_container>;
        { scanner.trailing_padding() } -> std::convertible_to<std::size_t>;
        { scanner.release_fragment(fragment_index) } noexcept
            -> std::same_as<void>;
    };

/** Stateless scanner tag for an already-resident compact sequence. */
template <class p_storage_unit>
struct resident_sequence_scanner final {
    using storage_unit = p_storage_unit;
    using base_container = std::span<const storage_unit>;
    static constexpr std::size_t fragment_storage_units =
        default_fragment_storage_units;
    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool is_fragmented() noexcept { return false; }
};
static_assert(sequence_scanner<resident_sequence_scanner<std::uint64_t>>);
static_assert(!fragmented_sequence_scanner<
              resident_sequence_scanner<std::uint64_t>>);

/**
 * Fragmented scanner over caller-owned packed storage.
 *
 * This implementation is useful for deterministic chunking and for any
 * in-process source whose lifetime already dominates the scanner.
 */
template <class p_storage_unit, std::size_t p_fragment_storage_units>
struct chunked_span_sequence_scanner final {
    using storage_unit = p_storage_unit;
    using base_container = std::span<const storage_unit>;
    static constexpr std::size_t fragment_storage_units =
        p_fragment_storage_units;
    static_assert(p_fragment_storage_units >= 1u);

    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool is_fragmented() noexcept { return true; }

    /// Build a chunked scanner over the storage of an existing in-memory
    /// compact_sequence_range. The backing span is `range`'s packed storage
    /// units. The caller owns those bytes for the scanner's lifetime. We
    /// derive the span from `read_cptr(cbegin())` + `size()` because the
    /// project's compact_sequence_range has no `raw_storage_span` accessor.
    template <class p_range>
    [[nodiscard]] static chunked_span_sequence_scanner
    from_range(const compact_sequence_range<p_range>& range) noexcept {
        using traits = typename compact_sequence_range<p_range>::traits;
        static_assert(
            std::is_same_v<typename traits::storage_unit, p_storage_unit>,
            "chunked_span_sequence_scanner: storage unit must match the "
            "range's traits::storage_unit");
        chunked_span_sequence_scanner scanner;
        const std::size_t units = static_cast<std::size_t>(range.size());
        if (units == 0) {
            scanner.backing_ = base_container{};
        } else {
            scanner.backing_ = base_container(
                range.read_cptr(range.cbegin()), units);
        }
        scanner.bp_size_ = static_cast<sequence_size_t>(range.bp_size());
        scanner.trailing_padding_ =
            static_cast<std::uint16_t>(range.padding());
        return scanner;
    }

    [[nodiscard]] sequence_size_t bp_size() const noexcept { return bp_size_; }
    [[nodiscard]] sequence_size_t storage_unit_count() const noexcept {
        return static_cast<sequence_size_t>(backing_.size());
    }
    [[nodiscard]] sequence_size_t storage_byte_count() const noexcept {
        const auto bytes = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(backing_.size()),
            static_cast<std::uint64_t>(sizeof(storage_unit)));
        if (!bytes.has_value()) {
            abort_lazy_sequence_access(
                "chunked scanner byte count is not representable");
        }
        return *bytes;
    }
    [[nodiscard]] std::size_t fragment_count() const noexcept {
        return backing_.size() / fragment_storage_units
            + static_cast<std::size_t>(
                backing_.size() % fragment_storage_units != 0u);
    }
    [[nodiscard]] sequence_size_t estimated_bp_size() const noexcept {
        return bp_size_;
    }
    [[nodiscard]] std::size_t trailing_padding() const noexcept {
        return trailing_padding_;
    }

    [[nodiscard]] base_container load_fragment(
        std::size_t fragment_index) const noexcept
    {
        if (fragment_index >= fragment_count()) {
            abort_lazy_sequence_access(
                "chunked span scanner fragment index is out of range");
        }
        const std::size_t start = fragment_index * fragment_storage_units;
        const std::size_t count = std::min(
            fragment_storage_units, backing_.size() - start);
        return backing_.subspan(start, count);
    }

    void release_fragment(std::size_t) const noexcept {}

    chunked_span_sequence_scanner(const chunked_span_sequence_scanner&) = delete;
    chunked_span_sequence_scanner& operator=(
        const chunked_span_sequence_scanner&) = delete;
    chunked_span_sequence_scanner(chunked_span_sequence_scanner&&) noexcept = default;
    chunked_span_sequence_scanner& operator=(
        chunked_span_sequence_scanner&&) noexcept = default;
    chunked_span_sequence_scanner() noexcept = default;
    ~chunked_span_sequence_scanner() = default;

private:
    std::span<const storage_unit> backing_;
    sequence_size_t bp_size_ = 0u;
    std::uint16_t trailing_padding_ = 0u;
};
static_assert(fragmented_sequence_scanner<
              chunked_span_sequence_scanner<std::uint64_t, 1024u>>);
static_assert(fragmented_sequence_scanner<
              chunked_span_sequence_scanner<std::uint64_t, 1u>>);

/** Structural contract implemented by concrete lazy-sequence owners. */
template <class p_view>
concept concrete_lazy_sequence = requires {
    typename p_view::scanner_type;
    typename p_view::range_type;
    typename p_view::iterator_impl;
    requires sequence_scanner<typename p_view::scanner_type>;
};

/** @brief Marks views that satisfy the logical lazy-sequence slice contract. */
struct lazy_sequence_slice_tag {};

/** @brief Contiguity guarantee required by a sequence consumer. */
enum class sequence_access_requirement : std::uint8_t {
    sequential_symbols,
    bounded_random_window,
    contiguous_resident,
};

template <typename p_view, bool p_is_reversed, bool p_write_reversed>
concept oriented_lazy_sequence_view =
    requires(p_view const& value,
             std::span<std::uint8_t> output,
             std::size_t from,
             std::size_t to) {
        { value.template copy_ranks<p_is_reversed, p_write_reversed>(
              output, from, to) } -> std::same_as<void>;
        {
            value.template rank_reader<p_is_reversed, p_write_reversed,
                                       std::uint8_t, 2>()
                 .read_at(sequence_size_t{from}, to - from, output)
        } -> std::same_as<std::uint8_t const*>;
    };

template <typename p_iterator>
concept logical_rank_iterator = requires(p_iterator cursor, p_iterator other) {
    { *cursor } -> std::convertible_to<std::uint8_t>;
    { ++cursor } -> std::same_as<p_iterator&>;
    { cursor == other } -> std::convertible_to<bool>;
};

template <typename p_range>
concept logical_rank_range = requires(p_range const& range) {
    { range.begin() };
    { range.end() } -> std::same_as<decltype(range.begin())>;
    // Existing compact ranges return iterator endpoints by const reference.
    // Validate the usable value type, not the accessor's reference qualifier.
    requires logical_rank_iterator<
        std::remove_cvref_t<decltype(range.begin())>>;
};

template <typename p_view>
concept lazy_sequence_view_contract =
    requires(p_view const& value) {
        typename p_view::traits;
        typename p_view::traits::alphabet_type;
        { value.bp_size() } -> std::convertible_to<sequence_size_t>;
        { value.begin() };
        { value.end() };
        { value.template iter<iterator_mode::logical_rank>(value.begin()) };
        { value.template iterable<iterator_mode::logical_rank>() };
        { is_worker_shareable(value) } -> std::convertible_to<bool>;
        requires logical_rank_iterator<std::remove_cvref_t<
            decltype(value.template iter<iterator_mode::logical_rank>(
                value.begin()))>>;
        requires logical_rank_range<std::remove_cvref_t<
            decltype(value.template iterable<iterator_mode::logical_rank>())>>;
    }
    && oriented_lazy_sequence_view<p_view, false, false>
    && oriented_lazy_sequence_view<p_view, false, true>
    && oriented_lazy_sequence_view<p_view, true, false>
    && oriented_lazy_sequence_view<p_view, true, true>;

template <typename p_view>
concept resident_packed_sequence_view =
    lazy_sequence_view_contract<p_view>
    && requires(p_view const& value,
                sequence_size_t from,
                sequence_size_t to,
                std::uint8_t width) {
        { value.try_resident_packed(from, to, width) }
            -> std::same_as<
                std::optional<typename p_view::resident_packed_sequence>>;
    };

template <typename p_view>
concept lazy_sequence_slice =
    lazy_sequence_view_contract<p_view>
    && requires {
        typename p_view::slice_tag;
        requires std::same_as<typename p_view::slice_tag,
                              lazy_sequence_slice_tag>;
    };

}  // namespace bioseq::storage
