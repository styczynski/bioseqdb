/**
 * @file storage/lazy_sequence_parts/adapters.cppm
 * @brief Adapts concrete sequence storage to the shared lazy view interface.
 */
export module bioseq.storage.lazy_sequence:adapters;

import std;
import bioseq.storage.lazy_sequence_view;
import :owners;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.storage.scanner;
import bioseq.storage.size;
import bioseq.sequences;

export namespace bioseq::storage {

namespace lazy_sequence_detail {

template <bool p_is_reversed, bool p_write_reversed, class p_sequence_range, class p_scanner>
[[nodiscard]] const std::uint8_t* owned_sequence_read_at(void const* state,
                                                         sequence_size_t bp_from,
                                                         std::size_t required_length,
                                                         std::span<std::uint8_t> scratch) {
    const auto& sequence = *static_cast<const lazy_sequence<p_sequence_range, p_scanner>*>(state);
    if (bp_from > sequence.bp_size() || required_length > sequence.bp_size() - bp_from ||
        scratch.size() < required_length) {
        abort_lazy_sequence_access("owned lazy sequence rank range is invalid");
    }
    if (required_length == 0u) {
        return scratch.data();
    }
    using alphabet_type = typename compact_sequence_range<p_sequence_range>::traits::alphabet_type;
    if constexpr (p_is_reversed && !complementable_sequence_alphabet<alphabet_type>) {
        throw std::invalid_argument(
            "reverse-complement encoding requires a complementable alphabet");
    } else {
        auto cursor =
            sequence.template rank_reader<p_is_reversed, p_write_reversed, std::uint8_t, 2>();
        const std::uint8_t* result = cursor.read_at(bp_from, required_length, scratch);
        // The local reader owns its decoded fragment buffers and dies on return.
        // Copy any borrowed ranks to caller scratch before that happens.
        if (result != scratch.data()) {
            std::memcpy(scratch.data(), result, required_length);
        }
        return scratch.data();
    }
}

template <class p_sequence_range, class p_scanner>
[[nodiscard]] sequence_size_t owned_sequence_bp_size(void const* state) noexcept {
    return static_cast<const lazy_sequence<p_sequence_range, p_scanner>*>(state)->bp_size();
}

template <class p_sequence_range, class p_scanner>
[[nodiscard]] bool owned_sequence_worker_shareable(void const* state) noexcept {
    const auto& sequence = *static_cast<const lazy_sequence<p_sequence_range, p_scanner>*>(state);
    return is_worker_shareable(sequence);
}

template <class p_sequence_range, class p_scanner>
[[nodiscard]] auto owned_sequence_resident_probe(void const* state,
                                                 sequence_size_t from,
                                                 sequence_size_t to,
                                                 std::uint8_t requested_width) noexcept ->
    typename lazy_sequence_view<typename compact_sequence_range<
        p_sequence_range>::traits::alphabet_type>::resident_packed_result {
    using range_traits = typename compact_sequence_range<p_sequence_range>::traits;
    using alphabet_type = typename range_traits::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;
    using descriptor_type = typename view_type::resident_packed_sequence;

    if constexpr (p_scanner::is_fragmented()) {
        // Later reads can evict fragment-cache slots, so their pointers cannot
        // be borrowed for the lifetime of the view.
        (void)state;
        (void)from;
        (void)to;
        (void)requested_width;
        return std::nullopt;
    } else {
        constexpr std::uint8_t k_physical_width =
            static_cast<std::uint8_t>(sizeof(typename range_traits::storage_unit));
        constexpr std::uint8_t k_logical_bits = static_cast<std::uint8_t>(
            lazy_sequence_traits<alphabet_type>::k_logical_bits_per_symbol);
        if (requested_width != k_physical_width || range_traits::bits_per_char != k_logical_bits) {
            return std::nullopt;
        }

        const sequence_size_t length = to - from;
        if (length == 0u) {
            return descriptor_type{
                .units = nullptr,
                .unit_count = 0u,
                .logical_length = 0u,
                .storage_unit_width = k_physical_width,
                .bits_per_symbol = k_logical_bits,
                .leading_symbol_offset = 0u,
                .trailing_padding = 0u,
                .bit_order = view_type::packed_symbol_order::least_significant_symbol_first,
            };
        }

        constexpr sequence_size_t k_letters_per_unit = range_traits::letters_per_unit;
        const sequence_size_t first_unit = from / k_letters_per_unit;
        const sequence_size_t leading = from % k_letters_per_unit;
        const auto occupied = bioseq::common::checked_add(leading, length);
        if (!occupied.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t unit_count = bioseq::common::ceil_div(*occupied, k_letters_per_unit);
        const auto represented_symbols =
            bioseq::common::checked_mul(unit_count, k_letters_per_unit);
        if (!represented_symbols.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t trailing = *represented_symbols - *occupied;

        const auto& sequence =
            *static_cast<const lazy_sequence<p_sequence_range, p_scanner>*>(state);
        const auto& range = sequence.underlying();
        if (first_unit > range.size() || unit_count > range.size() - first_unit ||
            unit_count > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
            return std::nullopt;
        }
        const auto* base = range.read_cptr(range.cbegin());
        return descriptor_type{
            .units = base + static_cast<std::size_t>(first_unit),
            .unit_count = static_cast<std::size_t>(unit_count),
            .logical_length = length,
            .storage_unit_width = k_physical_width,
            .bits_per_symbol = k_logical_bits,
            .leading_symbol_offset = static_cast<std::uint16_t>(leading),
            .trailing_padding = static_cast<std::uint16_t>(trailing),
            .bit_order = view_type::packed_symbol_order::least_significant_symbol_first,
        };
    }
}

template <class p_sequence_range, class p_scanner>
[[nodiscard]] bool owned_sequence_copy_packed(void const* state,
                                              void* destination,
                                              std::size_t unit_count,
                                              std::uint8_t requested_width) {
    using range_traits = typename compact_sequence_range<p_sequence_range>::traits;
    using storage_unit = typename range_traits::storage_unit;
    using alphabet_type = typename range_traits::alphabet_type;
    if constexpr (requires(const p_scanner& scanner, std::span<storage_unit> output) {
                      { scanner.copy_packed_to(output) } -> std::same_as<void>;
                  }) {
        constexpr std::uint8_t k_physical_width = static_cast<std::uint8_t>(sizeof(storage_unit));
        constexpr std::size_t k_logical_bits =
            lazy_sequence_traits<alphabet_type>::k_logical_bits_per_symbol;
        constexpr std::size_t k_letters_per_unit = range_traits::letters_per_unit;
        const auto& sequence =
            *static_cast<const lazy_sequence<p_sequence_range, p_scanner>*>(state);
        const sequence_size_t expected_units = bioseq::common::ceil_div(
            sequence.bp_size(), static_cast<sequence_size_t>(k_letters_per_unit));
        if (requested_width != k_physical_width || range_traits::bits_per_char != k_logical_bits ||
            expected_units >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)()) ||
            unit_count != static_cast<std::size_t>(expected_units) ||
            (unit_count != 0u && destination == nullptr) ||
            (unit_count != 0u &&
             reinterpret_cast<std::uintptr_t>(destination) % alignof(storage_unit) != 0u)) {
            return false;
        }
        sequence.scanner().copy_packed_to(
            std::span<storage_unit>{static_cast<storage_unit*>(destination), unit_count});
        return true;
    } else {
        (void)state;
        (void)destination;
        (void)unit_count;
        (void)requested_width;
        return false;
    }
}

/**
 * Copies native packed words for the requested window.
 *
 * Returns false for incompatible width, bounds, or destination alignment.
 * The destination must hold unit_count words. Fragmented reads use it as
 * scratch, while resident reads are copied from their existing storage.
 */
template <class p_sequence_range, class p_scanner>
[[nodiscard]] bool owned_sequence_copy_packed_window(void const* state,
                                                     void* destination,
                                                     std::size_t first_unit,
                                                     std::size_t unit_count,
                                                     std::uint8_t requested_width) {
    using range_traits = typename compact_sequence_range<p_sequence_range>::traits;
    using storage_unit = typename range_traits::storage_unit;
    using alphabet_type = typename range_traits::alphabet_type;
    constexpr std::uint8_t k_physical_width = static_cast<std::uint8_t>(sizeof(storage_unit));
    constexpr std::size_t k_logical_bits =
        lazy_sequence_traits<alphabet_type>::k_logical_bits_per_symbol;
    const auto& sequence = *static_cast<const lazy_sequence<p_sequence_range, p_scanner>*>(state);
    if (requested_width != k_physical_width || range_traits::bits_per_char != k_logical_bits ||
        first_unit > sequence.size() || unit_count > sequence.size() - first_unit ||
        (unit_count != 0u && destination == nullptr) ||
        (unit_count != 0u &&
         reinterpret_cast<std::uintptr_t>(destination) % alignof(storage_unit) != 0u)) {
        return false;
    }
    if (unit_count == 0u) {
        return true;
    }
    auto begin = sequence.cbegin();
    begin += static_cast<sequence_size_t>(first_unit);
    std::span<storage_unit> output{static_cast<storage_unit*>(destination), unit_count};
    const storage_unit* source = sequence.read_cptr(begin, output, unit_count);
    if (source != output.data()) {
        std::memcpy(output.data(), source, unit_count * sizeof(storage_unit));
    }
    return true;
}

template <class p_sequence_range, class p_scanner> struct owned_sequence_adapter {
    using alphabet_type = typename compact_sequence_range<p_sequence_range>::traits::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;

    static inline const typename view_type::backend_ops k_ops{
        .bp_size = &owned_sequence_bp_size<p_sequence_range, p_scanner>,
        .read_at =
            {
                &owned_sequence_read_at<false, false, p_sequence_range, p_scanner>,
                &owned_sequence_read_at<false, true, p_sequence_range, p_scanner>,
                &owned_sequence_read_at<true, false, p_sequence_range, p_scanner>,
                &owned_sequence_read_at<true, true, p_sequence_range, p_scanner>,
            },
        .try_resident_packed = &owned_sequence_resident_probe<p_sequence_range, p_scanner>,
        .copy_packed_sequence = &owned_sequence_copy_packed<p_sequence_range, p_scanner>,
        .copy_packed_window = &owned_sequence_copy_packed_window<p_sequence_range, p_scanner>,
        .worker_shareable = &owned_sequence_worker_shareable<p_sequence_range, p_scanner>,
    };
};

// Adapt compact ranges directly, borrowing the range and its storage.
// Callbacks create a temporary resident wrapper. Decoded ranks are copied
// to caller scratch, while packed probes borrow the range's own words.
template <bool p_is_reversed, bool p_write_reversed, class p_sequence_range>
[[nodiscard]] const std::uint8_t* compact_range_read_at(void const* state,
                                                        sequence_size_t bp_from,
                                                        std::size_t required_length,
                                                        std::span<std::uint8_t> scratch) {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<p_sequence_range>*>(state);
    const lazy_sequence<p_sequence_range, scanner_type> owner{range};
    return owned_sequence_read_at<p_is_reversed, p_write_reversed, p_sequence_range, scanner_type>(
        std::addressof(owner), bp_from, required_length, scratch);
}

template <class p_sequence_range>
[[nodiscard]] sequence_size_t compact_range_bp_size(void const* state) noexcept {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<p_sequence_range>*>(state);
    const lazy_sequence<p_sequence_range, scanner_type> owner{range};
    return owner.bp_size();
}

template <class p_sequence_range>
[[nodiscard]] bool compact_range_worker_shareable(void const*) noexcept {
    return true;
}

template <class p_sequence_range>
[[nodiscard]] auto compact_range_resident_probe(void const* state,
                                                sequence_size_t from,
                                                sequence_size_t to,
                                                std::uint8_t requested_width) noexcept ->
    typename lazy_sequence_view<typename compact_sequence_range<
        p_sequence_range>::traits::alphabet_type>::resident_packed_result {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<p_sequence_range>*>(state);
    const lazy_sequence<p_sequence_range, scanner_type> owner{range};
    return owned_sequence_resident_probe<p_sequence_range, scanner_type>(
        std::addressof(owner), from, to, requested_width);
}

/** Copies native words from a resident compact range into caller storage. */
template <class p_sequence_range>
[[nodiscard]] bool compact_range_copy_packed_window(void const* state,
                                                    void* destination,
                                                    std::size_t first_unit,
                                                    std::size_t unit_count,
                                                    std::uint8_t requested_width) {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<p_sequence_range>*>(state);
    const lazy_sequence<p_sequence_range, scanner_type> owner{range};
    return owned_sequence_copy_packed_window<p_sequence_range, scanner_type>(
        std::addressof(owner), destination, first_unit, unit_count, requested_width);
}

template <class p_sequence_range> struct compact_range_adapter {
    using alphabet_type = typename compact_sequence_range<p_sequence_range>::traits::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;

    static inline const typename view_type::backend_ops k_ops{
        .bp_size = &compact_range_bp_size<p_sequence_range>,
        .read_at =
            {
                &compact_range_read_at<false, false, p_sequence_range>,
                &compact_range_read_at<false, true, p_sequence_range>,
                &compact_range_read_at<true, false, p_sequence_range>,
                &compact_range_read_at<true, true, p_sequence_range>,
            },
        .try_resident_packed = &compact_range_resident_probe<p_sequence_range>,
        .copy_packed_sequence = nullptr,
        .copy_packed_window = &compact_range_copy_packed_window<p_sequence_range>,
        .worker_shareable = &compact_range_worker_shareable<p_sequence_range>,
    };
};

} // namespace lazy_sequence_detail

/** Borrows a concrete lazy owner through the shared view interface. The owner must outlive the view. */
template <class p_sequence_range, class p_scanner>
[[nodiscard]] inline auto
make_lazy_sequence_view(const lazy_sequence<p_sequence_range, p_scanner>& owner) noexcept {
    using adapter_type = lazy_sequence_detail::owned_sequence_adapter<p_sequence_range, p_scanner>;
    return typename adapter_type::view_type{std::addressof(owner),
                                            std::addressof(adapter_type::k_ops)};
}

namespace lazy_sequence_detail {

/**
 * Keeps a fragmented scanner at a stable address for borrowed lazy views.
 *
 * The owner cannot be moved. Its lifecycle policy provides separate operations
 * for finishing source resources after success and abandoning them during cleanup.
 */
template <class p_sequence_range, class p_scanner, class p_resource_lifecycle>
    requires(p_scanner::is_fragmented())
class fragmented_facade_owner final {
  public:
    using alphabet_type = typename compact_sequence_range<p_sequence_range>::traits::alphabet_type;

    /// Moves a scanner into this fixed-address owner.
    explicit fragmented_facade_owner(p_scanner&& scanner) noexcept : owner_(std::move(scanner)) {}

    fragmented_facade_owner(fragmented_facade_owner const&) = delete;
    fragmented_facade_owner& operator=(fragmented_facade_owner const&) = delete;
    fragmented_facade_owner(fragmented_facade_owner&&) = delete;
    fragmented_facade_owner& operator=(fragmented_facade_owner&&) = delete;

    /// Returns a borrowed view valid while the owner and its source resources remain alive.
    [[nodiscard]] lazy_sequence_view<alphabet_type> view() const noexcept {
        return make_lazy_sequence_view(owner_);
    }

    /// Runs the source policy's resource cleanup for a successful operation.
    void finish_resources_as_cpp() const
        requires requires(p_scanner const& scanner) {
            { p_resource_lifecycle::finish_as_cpp(scanner) } -> std::same_as<void>;
        }
    {
        p_resource_lifecycle::finish_as_cpp(owner_.scanner());
    }

    /// Calls the source policy to abandon handles without throwing.
    void abandon_resources_noexcept() const noexcept
        requires requires(p_scanner const& scanner) {
            { p_resource_lifecycle::abandon_noexcept(scanner) } noexcept -> std::same_as<void>;
        }
    {
        p_resource_lifecycle::abandon_noexcept(owner_.scanner());
    }

  private:
    lazy_sequence<p_sequence_range, p_scanner> owner_;
};

} // namespace lazy_sequence_detail

/** Borrows a resident compact range through the shared view interface. */
template <class p_sequence_range>
[[nodiscard]] inline auto
make_lazy_sequence_view(const compact_sequence_range<p_sequence_range>& owner) noexcept {
    using adapter_type = lazy_sequence_detail::compact_range_adapter<p_sequence_range>;
    return typename adapter_type::view_type{std::addressof(owner),
                                            std::addressof(adapter_type::k_ops)};
}

/**
 * Calls callback with a view that reads the range in fixed-size fragments.
 *
 * The temporary scanner and owner live until the callback returns. The callback
 * must not keep the view after returning. The range continues to own or borrow
 * its original packed storage.
 */
template <std::size_t p_fragment_storage_units = default_fragment_storage_units,
          class p_sequence_range,
          class p_callback>
inline void with_chunked_lazy_sequence(const compact_sequence_range<p_sequence_range>& range,
                                       p_callback&& callback) {
    using scanner_type = chunked_span_sequence_scanner<
        typename compact_sequence_range<p_sequence_range>::traits::storage_unit,
        p_fragment_storage_units>;
    lazy_sequence<p_sequence_range, scanner_type> owner{scanner_type::from_range(range)};
    std::forward<p_callback>(callback)(make_lazy_sequence_view(owner));
}

/** Keeps two fragment readers alive while callback uses their borrowed views. */
template <std::size_t p_fragment_storage_units = default_fragment_storage_units,
          class p_left_sequence_range,
          class p_right_sequence_range,
          class p_callback>
inline void
with_chunked_lazy_sequence(const compact_sequence_range<p_left_sequence_range>& left_range,
                           const compact_sequence_range<p_right_sequence_range>& right_range,
                           p_callback&& callback) {
    using left_alphabet =
        typename compact_sequence_range<p_left_sequence_range>::traits::alphabet_type;
    using right_alphabet =
        typename compact_sequence_range<p_right_sequence_range>::traits::alphabet_type;
    static_assert(std::is_same_v<left_alphabet, right_alphabet>,
                  "chunked facade pair must share one alphabet");
    with_chunked_lazy_sequence<p_fragment_storage_units>(
        left_range, [&right_range, &callback](auto left_view) {
            with_chunked_lazy_sequence<p_fragment_storage_units>(
                right_range, [left_view, &callback](auto right_view) {
                    std::forward<p_callback>(callback)(left_view, right_view);
                });
        });
}

} // namespace bioseq::storage
