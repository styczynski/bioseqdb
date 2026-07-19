/*-------------------------------------------------------------------------
 *
 * storage/sequence_scanner.cppm
 *
 * The `sequence_scanner` concept and built-in scanners (DOC0263 §3).
 *
 * A scanner is the compile-time-fixed back-end that produces sequence
 * fragments on demand for `lazy_compact_sequence<R, Scanner>`. Phase B ships:
 *   - sequence_scanner_none<storage_unit_t>           — the zero-overhead
 *     no-op default (supports_fragments() == false). Production uses this.
 *   - sequence_scanner_test_chunked<unit, N>          — a synthetic in-memory
 *     scanner that slices an existing compact_sequence_range into N-unit
 *     fragments, used by the parity tests (§11).
 * Phase C ships the production fragmented back-end `sequence_scanner_pg<S>`
 * against this same concept (DOC0264) — NOT here.
 *
 * DOC0263 §2.2 names this `bioseq_storage:sequence_scanner` (a partition of
 * bioseq_storage). C++ module partitions can only be imported from within the
 * same module, and `bioseq_storage`'s primary unit re-exports the
 * Postgres-only storage-manager partition, so a partition here would be
 * unreachable from the NO_POSTGRES algorithm modules (simd, the aligners,
 * pipe, …). We therefore ship it as a STANDALONE NO_POSTGRES module; the
 * public types still live in `bioseq::storage` exactly as the spec names them.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <array>
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

import bioseq.common.mlogging;
import bioseq.storage.size;
import bioseq.sequences;

static_assert(__cplusplus >= 202302L, "DOC0263 requires C++23-or-later");

export namespace bioseq::storage {

/// All scanners produce fragments in this alignment. 1024 storage units →
/// 8 KiB on uint64 → 32 768 letters per fragment for dna4/uint64 (DOC0263 §3).
constexpr std::size_t SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT = 1024;

/// Hard-error helper for the fragmented (test / Phase-C) path. DOC0263 R8
/// calls this `pg_error_panic_throw`: under Postgres it would `bioseq_log<ERR, PG>()`
/// (longjmp). The Phase-B fragmented path is compiled into the NO_POSTGRES
/// shared library, so we log via bioseq_log<DBG> and abort. Production uses the
/// no-fragment specialisation, which never reaches any panic site.
[[noreturn]] [[gnu::cold]] inline void lazy_fragment_panic(std::string_view message) {
    bioseq_log<DBG>("[lazy_compact_sequence] PANIC: {}", message);
    std::abort();
}

/// A POD describing one resident fragment. Owned by lazy_compact_sequence's
/// fragment_cache; the scanner produces the bytes, the wrapper holds the
/// metadata.
template <class R>
struct sequence_fragment_handle {
    using storage_unit = typename compact_sequence_range<R>::traits::storage_unit;
    std::span<const storage_unit> _data;                                         // points into scanner-owned memory
    sequence_size_t               _global_bp_offset = 0;
    sequence_size_t               _bp_letters       = 0;                          // letters in this fragment
    std::size_t                   _fragment_no      = std::numeric_limits<std::size_t>::max();
    std::uint16_t                 _padding          = 0;                          // 0 on non-final; trailing pad on final
};

/// Minimal concept every scanner must satisfy. Used to instantiate
/// lazy_compact_sequence's NO-FRAGMENT specialisation (which gates only on
/// `supports_fragments() == false`).
template <class S>
concept is_valid_sequence_scanner = requires {
    typename S::storage_unit;
    typename S::base_container;
    { S::supports_fragments() } -> std::same_as<bool>;
    { S::fragment_storage_units } -> std::convertible_to<std::size_t>;
};

/// Active scanners (= fragmented). Required by lazy_compact_sequence's
/// FRAGMENTED specialisation.
template <class S>
concept is_active_sequence_scanner = is_valid_sequence_scanner<S>
    && S::supports_fragments()
    && requires(S s, std::size_t n) {
        { s.total_bp_size()                 } -> std::convertible_to<sequence_size_t>;
        { s.total_size()                    } -> std::convertible_to<sequence_size_t>;
        { s.total_byte_size()               } -> std::convertible_to<sequence_size_t>;
        { s.number_of_fragments()           } -> std::convertible_to<std::size_t>;
        { s.estimated_total_bp_size()       } -> std::convertible_to<sequence_size_t>;
        { s.read_fragment(n)                } -> std::same_as<typename S::base_container>;
        { s.last_fragment_padding_letters() } -> std::convertible_to<std::size_t>;
    };

// ---- sequence_scanner_none — the no-op default --------------------------

template <class storage_unit_t>
struct sequence_scanner_none final {
    using storage_unit   = storage_unit_t;
    using base_container = std::span<const storage_unit>;
    static constexpr std::size_t fragment_storage_units =
        SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT;
    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool supports_fragments() noexcept { return false; }
};
static_assert(is_valid_sequence_scanner<sequence_scanner_none<std::uint64_t>>);
static_assert(!is_active_sequence_scanner<sequence_scanner_none<std::uint64_t>>);

// ---- sequence_scanner_test_chunked — the synthetic test back-end --------

template <class storage_unit_t, std::size_t p_fragment_storage_units>
struct sequence_scanner_test_chunked final {
    using storage_unit   = storage_unit_t;
    using base_container = std::span<const storage_unit>;
    /// Class member — NOT a template NTTP (consolidated-review C16).
    static constexpr std::size_t fragment_storage_units = p_fragment_storage_units;
    static_assert(p_fragment_storage_units >= 1);

    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool supports_fragments() noexcept { return true; }

    /// Build a chunked scanner over the storage of an existing in-memory
    /// compact_sequence_range. The backing span is `range`'s packed storage
    /// units; the caller owns those bytes for the scanner's lifetime. We
    /// derive the span from `read_cptr(cbegin())` + `size()` because the
    /// project's compact_sequence_range has no `raw_storage_span` accessor.
    template <class R>
    [[nodiscard]] static sequence_scanner_test_chunked
    from_range(const compact_sequence_range<R>& range) noexcept {
        using traits = typename compact_sequence_range<R>::traits;
        static_assert(std::is_same_v<typename traits::storage_unit, storage_unit_t>,
            "sequence_scanner_test_chunked: storage_unit_t must match the "
            "range's traits::storage_unit");
        sequence_scanner_test_chunked s;
        const std::size_t units = static_cast<std::size_t>(range.size());
        if (units == 0) {
            s._backing = base_container{};
        } else {
            s._backing = base_container(range.read_cptr(range.cbegin()), units);
        }
        s._total_bp_size    = static_cast<sequence_size_t>(range.bp_size());
        s._total_padding    = static_cast<std::uint16_t>(range.padding());
        s._letters_per_unit = static_cast<std::uint8_t>(traits::letters_per_unit);
        return s;
    }

    [[nodiscard]] sequence_size_t total_bp_size() const noexcept { return _total_bp_size; }
    [[nodiscard]] sequence_size_t total_size()    const noexcept {
        return static_cast<sequence_size_t>(_backing.size());
    }
    [[nodiscard]] sequence_size_t total_byte_size() const noexcept {
        return static_cast<sequence_size_t>(_backing.size()) * sizeof(storage_unit_t);
    }
    [[nodiscard]] std::size_t number_of_fragments() const noexcept {
        return (_backing.size() + fragment_storage_units - 1) / fragment_storage_units;
    }
    [[nodiscard]] sequence_size_t estimated_total_bp_size() const noexcept { return _total_bp_size; }
    [[nodiscard]] std::size_t last_fragment_padding_letters() const noexcept { return _total_padding; }

    [[nodiscard]] base_container read_fragment(std::size_t n) const noexcept {
        if (n >= number_of_fragments()) {
            lazy_fragment_panic("test scanner: read_fragment(n) out of range");
        }
        const std::size_t start = n * fragment_storage_units;
        const std::size_t end   = std::min(start + fragment_storage_units, _backing.size());
        return _backing.subspan(start, end - start);
    }

    /// Optional Phase-B release hook (matches the Phase-C scanner_pg API). For
    /// the synthetic test scanner this is a no-op because the backing span is
    /// caller-owned.
    void release_fragment(std::size_t /*n*/) const noexcept {}

    // Non-copyable, movable (consolidated-review R3 for scanners).
    sequence_scanner_test_chunked(const sequence_scanner_test_chunked&)            = delete;
    sequence_scanner_test_chunked& operator=(const sequence_scanner_test_chunked&) = delete;
    sequence_scanner_test_chunked(sequence_scanner_test_chunked&&) noexcept            = default;
    sequence_scanner_test_chunked& operator=(sequence_scanner_test_chunked&&) noexcept = default;
    sequence_scanner_test_chunked() noexcept = default;
    ~sequence_scanner_test_chunked() = default;

private:
    std::span<const storage_unit> _backing;
    sequence_size_t               _total_bp_size    = 0;
    std::uint16_t                 _total_padding    = 0;
    std::uint8_t                  _letters_per_unit = 0;
};
static_assert(is_active_sequence_scanner<sequence_scanner_test_chunked<std::uint64_t, 1024>>);
static_assert(is_active_sequence_scanner<sequence_scanner_test_chunked<std::uint64_t, 1>>);

/// DOC0263 §3.4 — the concept new algorithms must satisfy to consume a
/// lazy_compact_sequence. Defined here (rather than in the lazy module) so
/// algorithm modules can constrain on it without a heavier include.
template <class V>
concept supports_lazy_compact_sequence = requires {
    typename V::scanner_type;
    typename V::range_type;
    typename V::iterator_impl;
    requires is_valid_sequence_scanner<typename V::scanner_type>;
};

// DOC0377 §9.2 — behavioral view contracts. The legacy structural concept
// above intentionally remains source-compatible; migrated algorithms constrain
// on these operations instead of representation-specific nested types.
struct lazy_compact_sequence_slice_tag {};

enum class sequence_access_requirement : std::uint8_t {
    sequential_symbols,
    bounded_random_window,
    contiguous_resident,
};

template <typename V, bool IsReversed, bool WriteReversed>
concept supports_lazy_orientation =
    requires(V const& value,
             std::span<std::uint8_t> output,
             std::size_t from,
             std::size_t to) {
        { value.template to_encoded_into<IsReversed, WriteReversed>(
              output, from, to) } -> std::same_as<void>;
        {
            value.template encoded_iter<IsReversed, WriteReversed,
                                        std::uint8_t, 2>()
                 .ptr_at(sequence_size_t{from}, to - from, output)
        } -> std::same_as<std::uint8_t const*>;
    };

template <typename I>
concept supports_bp_iterator = requires(I cursor, I other) {
    { *cursor } -> std::convertible_to<std::uint8_t>;
    { ++cursor } -> std::same_as<I&>;
    { cursor == other } -> std::convertible_to<bool>;
};

template <typename R>
concept supports_bp_range = requires(R const& range) {
    { range.begin() };
    { range.end() } -> std::same_as<decltype(range.begin())>;
    // Existing compact ranges return iterator endpoints by const reference.
    // Validate the usable value type, not the accessor's reference qualifier.
    requires supports_bp_iterator<
        std::remove_cvref_t<decltype(range.begin())>>;
};

template <typename V>
concept supports_lazy_sequence_view =
    requires(V const& value) {
        typename V::traits;
        typename V::traits::alphabet_type;
        { value.bp_size() } -> std::convertible_to<sequence_size_t>;
        { value.begin() };
        { value.end() };
        { value.template iter<BP>(value.begin()) };
        { value.template iterable<BP>() };
        { lazy_is_worker_shareable(value) } -> std::convertible_to<bool>;
        requires supports_bp_iterator<std::remove_cvref_t<
            decltype(value.template iter<BP>(value.begin()))>>;
        requires supports_bp_range<std::remove_cvref_t<
            decltype(value.template iterable<BP>())>>;
    }
    && supports_lazy_orientation<V, false, false>
    && supports_lazy_orientation<V, false, true>
    && supports_lazy_orientation<V, true, false>
    && supports_lazy_orientation<V, true, true>;

template <typename V>
concept supports_resident_packed_probe =
    supports_lazy_sequence_view<V>
    && requires(V const& value,
                sequence_size_t from,
                sequence_size_t to,
                std::uint8_t width) {
        { value.try_resident_packed(from, to, width) }
            -> std::same_as<
                std::optional<typename V::resident_packed_region>>;
    };

template <typename V>
concept supports_lazy_compact_sequence_slice =
    supports_lazy_sequence_view<V>
    && requires {
        typename V::lazy_slice_tag;
        requires std::same_as<typename V::lazy_slice_tag,
                              lazy_compact_sequence_slice_tag>;
    };

}  // namespace bioseq::storage
