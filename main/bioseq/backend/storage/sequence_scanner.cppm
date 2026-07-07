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

}  // namespace bioseq::storage
