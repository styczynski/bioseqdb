/*
 * DOC0260 §4 / §5.3 / §12.1 — PG-free bucket math + locator POD.
 *
 * The length-bucket cut-point table, the log2 helper, the branchless
 * L->bucket LUT, and the `seq_locator` POD are pure, Postgres-free
 * code. They used to live inside `bioseq_storage:sequence_storage_manager`,
 * but that module imports the Postgres catalog/relation machinery, so a
 * unit-test TU could not link it. DOC0260 §12.1 requires a unit-test TU for
 * the bucket math + locator round-trip, so the pure pieces are factored out
 * here into a standalone module that the no-Postgres test executable can
 * import directly. `bioseq_storage` re-exports this module, so every existing
 * `import bioseq_storage` consumer (access_method, dispatch_buffer, …) still
 * sees `bioseq::storage::seq_locator` and the bucket-math helpers unchanged.
 *
 * The bioseq::pg_types::seq-varlena framing of the stub (is_locator_stub /
 * encode_locator_into_stub / decode_locator_from_stub) stays in
 * sequence_storage_manager because it touches the Postgres varlena layout.
 */
module;

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

export module bioseq.storage.bucket_math;

export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// DOC0371 — Phase C v3 locator POD. Replaces the Phase C v2 32-byte
// (version, bucket_idx, _pad_2, shard_idx, total_bp_size, sub_row_no) layout.
// Standard-layout so it can be memcpy'd in and out of the bioseq::pg_types::seq stub `data[]`
// region by the encode/decode helpers (which live in sequence_storage_manager).
//
// Layout (40 bytes):
//   offset 0  version          (1 byte)  = kLocatorVersion = 3
//   offset 1  bucket_idx       (1 byte)
//   offset 2  _pad_2           (2 bytes) — always 0
//   offset 4  shard_idx        (4 bytes)
//   offset 8  num_fragments    (4 bytes) — fragments in this sequence (uint32)
//   offset 12 _pad_4           (4 bytes) — always 0
//   offset 16 total_bp_size    (8 bytes) — full sequence bp count
//   offset 24 first_sub_row_no (8 bytes) — row no of fragment[0] in the shard
//   offset 32 source_epoch     (8 bytes) — immutable PostgreSQL-owned content epoch
// ---------------------------------------------------------------------------

constexpr std::uint8_t kLocatorVersion = 3;
inline constexpr std::uint8_t k_seq_locator_stub_marker = 0x80u;

[[nodiscard]] constexpr bool
is_seq_locator_stub_width(std::uint8_t width) noexcept {
    return (width & k_seq_locator_stub_marker) != 0u;
}

struct seq_locator {
    std::uint8_t    version = kLocatorVersion;
    std::uint8_t    bucket_idx = 0;
    std::uint16_t   _pad_2 = 0;
    std::uint32_t   shard_idx = 0;
    std::uint32_t   num_fragments = 0;
    std::uint32_t   _pad_4 = 0;
    std::uint64_t   total_bp_size = 0;
    std::uint64_t   first_sub_row_no = 0;
    std::uint64_t   source_epoch = 0;
};
static_assert(sizeof(seq_locator) == 40,
              "DOC0371: seq_locator v3 layout must stay 40 bytes.");
static_assert(std::is_standard_layout_v<seq_locator>,
              "seq_locator must be standard-layout (memcpy'd in/out of bioseq::pg_types::seq).");
static_assert(std::is_trivially_copyable_v<seq_locator>,
              "seq_locator must remain trivially copyable for its frozen v3 payload.");
static_assert(offsetof(seq_locator, num_fragments) == 8,
              "seq_locator::num_fragments offset must stay 8 bytes.");
static_assert(offsetof(seq_locator, total_bp_size) == 16,
              "DOC0264 §5: seq_locator::total_bp_size offset must stay 16 bytes "
              "(pg_compact_sequence.cppm reads at this offset).");
static_assert(offsetof(seq_locator, first_sub_row_no) == 24,
              "DOC0264 §5: seq_locator::first_sub_row_no offset must stay 24 bytes.");
static_assert(offsetof(seq_locator, source_epoch) == 32,
              "DOC0371: seq_locator::source_epoch offset must stay 32 bytes.");
// DOC0264 R13 — guarantee num_fragments (uint32) × max-fragment-bp covers the
// 1 Tbp project pessimistic ceiling.
static_assert((static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
               * std::uint64_t{32768})
              >= std::uint64_t{1'000'000'000'000ULL},
              "DOC0264 R13: v2 num_fragments capacity insufficient for 1 Tbp ceiling");

/// Encode the frozen v3 payload without owning PostgreSQL framing.  This keeps
/// the existing host-byte-order on-disk bytes unchanged; callers supply an
/// exact-size span and add any surrounding varlena header themselves.
[[nodiscard]] inline bool encode_seq_locator_payload(
    std::span<std::byte> output,
    seq_locator const& locator) noexcept
{
    if (output.size() != sizeof(seq_locator)
        || locator.version != kLocatorVersion
        || locator._pad_2 != 0u
        || locator._pad_4 != 0u) {
        return false;
    }
    std::memcpy(output.data(), std::addressof(locator), sizeof(locator));
    return true;
}

/// Decode and validate the frozen v3 payload.  Ordinary fields, including a
/// zero source_epoch, deliberately remain in the PG-free codec domain; the PG
/// storage framing layer applies its stronger live-source policy.
[[nodiscard]] inline std::optional<seq_locator> decode_seq_locator_payload(
    std::span<std::byte const> payload) noexcept
{
    if (payload.size() != sizeof(seq_locator)) {
        return std::nullopt;
    }
    seq_locator locator{};
    std::memcpy(std::addressof(locator), payload.data(), sizeof(locator));
    if (locator.version != kLocatorVersion
        || locator._pad_2 != 0u
        || locator._pad_4 != 0u) {
        return std::nullopt;
    }
    return locator;
}

// A locator may come from a raw on-disk stub, so every consumer that intends
// to rebuild one contiguous bioseq::pg_types::seq must validate the complete narrowing and
// row-range contract before opening a shard or doing allocation arithmetic.
// Keep the classification PostgreSQL-free so the exact corrupt boundaries are
// exhaustively testable without manufacturing unsafe varlena values.
enum class flat_locator_validation_error : std::uint8_t {
    none = 0u,
    invalid_wire_metadata,
    logical_length_exceeds_bigint,
    logical_length_exceeds_flat_field,
    shard_exceeds_catalog_domain,
    first_row_exceeds_catalog_domain,
    fragment_geometry_inconsistent,
    fragment_row_range_exceeds_catalog_domain,
};

[[nodiscard]] constexpr flat_locator_validation_error
validate_flat_locator_geometry(seq_locator const& locator) noexcept
{
    constexpr auto bigint_max =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (locator.version != kLocatorVersion
        || locator._pad_2 != 0u
        || locator._pad_4 != 0u
        || locator.source_epoch == 0u
        || locator.source_epoch > bigint_max) {
        return flat_locator_validation_error::invalid_wire_metadata;
    }
    if (locator.total_bp_size > bigint_max) {
        return flat_locator_validation_error::logical_length_exceeds_bigint;
    }
    if (locator.total_bp_size
        > static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return flat_locator_validation_error::logical_length_exceeds_flat_field;
    }
    if (locator.shard_idx
        > static_cast<std::uint32_t>(
            (std::numeric_limits<std::int32_t>::max)())) {
        return flat_locator_validation_error::shard_exceeds_catalog_domain;
    }
    if (locator.first_sub_row_no > bigint_max) {
        return flat_locator_validation_error::first_row_exceeds_catalog_domain;
    }
    if ((locator.num_fragments == 0u) != (locator.total_bp_size == 0u)
        || (locator.num_fragments == 0u
            && locator.first_sub_row_no != 0u)) {
        return flat_locator_validation_error::fragment_geometry_inconsistent;
    }
    if (locator.num_fragments > 0u
        && locator.first_sub_row_no
            > bigint_max
                - static_cast<std::uint64_t>(locator.num_fragments - 1u)) {
        return flat_locator_validation_error::
            fragment_row_range_exceeds_catalog_domain;
    }
    return flat_locator_validation_error::none;
}

[[nodiscard]] constexpr std::uint64_t
expected_flat_locator_fragment_count(
    std::uint64_t total_bp_size,
    std::uint64_t letters_per_unit,
    std::uint64_t fragment_storage_units) noexcept
{
    if (total_bp_size == 0u) {
        return 0u;
    }
    if (letters_per_unit == 0u || fragment_storage_units == 0u) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    const std::uint64_t total_units =
        total_bp_size / letters_per_unit
        + (total_bp_size % letters_per_unit != 0u ? 1u : 0u);
    return total_units / fragment_storage_units
        + (total_units % fragment_storage_units != 0u ? 1u : 0u);
}

enum class fragment_frame_validation_error : std::uint8_t {
    none = 0u,
    shorter_than_header,
    size_mismatch,
};

[[nodiscard]] constexpr fragment_frame_validation_error
validate_fragment_frame_size(
    std::uint64_t actual_bytes,
    std::uint64_t header_bytes,
    std::uint64_t expected_bytes) noexcept
{
    if (actual_bytes < header_bytes) {
        return fragment_frame_validation_error::shorter_than_header;
    }
    if (actual_bytes != expected_bytes) {
        return fragment_frame_validation_error::size_mismatch;
    }
    return fragment_frame_validation_error::none;
}

enum class fragment_metadata_validation_error : std::uint8_t {
    none = 0u,
    type_mismatch,
    logical_length_mismatch,
    padding_mismatch,
};

[[nodiscard]] constexpr fragment_metadata_validation_error
validate_fragment_metadata(
    std::uint8_t actual_alphabet,
    std::uint8_t actual_width,
    std::uint64_t actual_bp_size,
    std::uint64_t actual_padding,
    std::uint8_t expected_alphabet,
    std::uint8_t expected_width,
    std::uint64_t expected_bp_size,
    std::uint64_t expected_padding) noexcept
{
    if (actual_alphabet != expected_alphabet
        || actual_width != expected_width) {
        return fragment_metadata_validation_error::type_mismatch;
    }
    if (actual_bp_size != expected_bp_size) {
        return fragment_metadata_validation_error::logical_length_mismatch;
    }
    if (actual_padding != expected_padding) {
        return fragment_metadata_validation_error::padding_mismatch;
    }
    return fragment_metadata_validation_error::none;
}

// ---------------------------------------------------------------------------
// §4 — bucket cut-points. One bucket for L in [0, min_bits], one bucket each
// for L in {min_bits+1 .. max_bits-1}, one bucket for L in [max_bits,
// total_len_bits]. For <uint64_t, 8, 42> that is 1 + 33 + 1 = 35 buckets.
// ---------------------------------------------------------------------------

template<class len_t,
         std::size_t bucket_min_len_bits,
         std::size_t bucket_max_len_bits>
[[nodiscard]] consteval auto generate_seq_buckets_len_thresholds() {
    static_assert(bucket_min_len_bits < bucket_max_len_bits,
                  "min bits must be strictly less than max bits");
    static_assert(bucket_max_len_bits <= sizeof(len_t) * 8u,
                  "max bits must fit in len_t");

    constexpr std::size_t total_len_bits = sizeof(len_t) * 8u;
    constexpr std::size_t single_count   = bucket_max_len_bits - bucket_min_len_bits - 1u;
    constexpr std::size_t bucket_count   = 1u + single_count + 1u;

    std::array<std::size_t, bucket_count> out{};
    out[0] = bucket_min_len_bits;
    for (std::size_t i = 0; i < single_count; ++i) {
        out[i + 1] = bucket_min_len_bits + 1u + i;
    }
    out[bucket_count - 1u] = total_len_bits;
    return out;
}

static_assert(generate_seq_buckets_len_thresholds<std::uint64_t, 8, 42>().size() == 35);
static_assert(generate_seq_buckets_len_thresholds<std::uint64_t, 8, 42>()[0]  == 8);
static_assert(generate_seq_buckets_len_thresholds<std::uint64_t, 8, 42>()[33] == 41);
static_assert(generate_seq_buckets_len_thresholds<std::uint64_t, 8, 42>()[34] == 64);

[[nodiscard]] [[gnu::always_inline]]
constexpr std::size_t len_log2_for_size(std::uint64_t seq_size) noexcept {
    const std::uint64_t need = seq_size > 1u ? seq_size - 1u : 1u;
    return static_cast<std::size_t>(std::bit_width(need));
}

// ---------------------------------------------------------------------------
// §7.2 — the branchless L->bucket LUT, materialised once per
// (len_t, min, max) instantiation as a variable template, plus the
// bucket_index_for_size entry point. sequence_storage_manager's template
// method delegates here so production and the §12.1 unit test resolve to the
// exact same code.
// ---------------------------------------------------------------------------

template<class len_t, std::size_t min_bits, std::size_t max_bits>
[[nodiscard]] consteval auto build_bucket_lut() {
    constexpr std::size_t total_len_bits = sizeof(len_t) * 8u;
    constexpr auto thresholds =
        generate_seq_buckets_len_thresholds<len_t, min_bits, max_bits>();
    constexpr std::size_t bucket_count = thresholds.size();
    std::array<std::uint8_t, total_len_bits + 1u> out{};
    for (std::size_t L = 0; L <= total_len_bits; ++L) {
        std::size_t b = bucket_count - 1u;
        for (std::size_t i = 0; i < bucket_count; ++i) {
            if (L <= thresholds[i]) {
                b = i;
                break;
            }
        }
        out[L] = static_cast<std::uint8_t>(b);
    }
    return out;
}

template<class len_t, std::size_t min_bits, std::size_t max_bits>
inline constexpr auto bucket_lut_v = build_bucket_lut<len_t, min_bits, max_bits>();

template<class len_t, std::size_t min_bits, std::size_t max_bits>
inline constexpr std::size_t bucket_count_v =
    generate_seq_buckets_len_thresholds<len_t, min_bits, max_bits>().size();

static_assert(bucket_count_v<std::uint64_t, 8, 42>
              <= std::numeric_limits<std::uint8_t>::max(),
              "seq_locator::bucket_idx is uint8_t; raise the locator format "
              "version before configuring more than 255 buckets");

template<class len_t, std::size_t min_bits, std::size_t max_bits>
[[nodiscard]] [[gnu::always_inline]] constexpr std::size_t
bucket_index_for_size(std::uint64_t seq_size) noexcept {
    constexpr std::size_t total_len_bits = sizeof(len_t) * 8u;
    const std::size_t L = len_log2_for_size(seq_size);
    const std::size_t L_clamped = L > total_len_bits ? total_len_bits : L;
    return static_cast<std::size_t>(
        bucket_lut_v<len_t, min_bits, max_bits>[L_clamped]);
}

}  // namespace bioseq::storage
