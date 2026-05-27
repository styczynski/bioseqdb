/*
 * DOC0260 §4 / §5.3 / §12.1 — PG-free bucket math + locator POD.
 *
 * The length-bucket cut-point table, the log2 helper, the branchless
 * L->bucket LUT, and the 24-byte `seq_locator` POD are pure, Postgres-free
 * code. They used to live inside `bioseq_storage:sequence_storage_manager`,
 * but that module imports the Postgres catalog/relation machinery, so a
 * unit-test TU could not link it. DOC0260 §12.1 requires a unit-test TU for
 * the bucket math + locator round-trip, so the pure pieces are factored out
 * here into a standalone module that the no-Postgres test executable can
 * import directly. `bioseq_storage` re-exports this module, so every existing
 * `import bioseq_storage` consumer (access_method, dispatch_buffer, …) still
 * sees `bioseq::storage::seq_locator` and the bucket-math helpers unchanged.
 *
 * The pgseq_t-varlena framing of the stub (is_locator_stub /
 * encode_locator_into_stub / decode_locator_from_stub) stays in
 * sequence_storage_manager because it touches the Postgres varlena layout.
 */
module;

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module bioseq_bucket_math;

export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// §5.3 — the 24-byte locator POD. Standard-layout so it can be memcpy'd in and
// out of the pgseq_t stub `data[]` region by the encode/decode helpers (which
// live in sequence_storage_manager).
// ---------------------------------------------------------------------------

constexpr std::uint8_t kLocatorVersion = 1;

struct seq_locator {
    std::uint8_t    version = kLocatorVersion;
    std::uint8_t    bucket_idx = 0;
    std::uint16_t   _pad_2 = 0;
    std::uint32_t   shard_idx = 0;
    std::uint64_t   total_bp_size = 0;
    std::uint64_t   sub_row_no = 0;
};
static_assert(sizeof(seq_locator) == 24,
              "seq_locator layout must stay 24 bytes (DOC0260 §5.3).");
static_assert(std::is_standard_layout_v<seq_locator>,
              "seq_locator must be standard-layout (memcpy'd in/out of pgseq_t).");
static_assert(offsetof(seq_locator, total_bp_size) == 8,
              "seq_locator::total_bp_size offset must stay 8 bytes "
              "(pg_compact_sequence.cppm reads at this offset).");
static_assert(offsetof(seq_locator, sub_row_no) == 16,
              "seq_locator::sub_row_no offset must stay 16 bytes.");

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
