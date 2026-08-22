/**
 * @file
 * Converts signed 128-bit aggregate values to PostgreSQL Numeric storage.
 */

/*-------------------------------------------------------------------------
 *
 * vectorization/types/numeric.cppm
 *
 * Mini Numeric arithmetic ported from PostgreSQL core so the columnar
 * vectorised aggregate path can build Numeric return values (e.g. int8 SUM,
 * AVG) without calling into PostgreSQL's internal numeric_* helpers.
 *
 * Only the pieces needed by the int128 -> Numeric conversion are kept.
 * The internal Numeric layout types live in the global module fragment so
 * they share linkage with PG's own forward-declared `struct NumericData`.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace {

constexpr int numeric_digit_base = 10'000;
constexpr int decimal_digits_per_numeric_digit = 4;

using numeric_digit = std::int16_t;

} // namespace

/**
 * Short form of an on-disk Numeric: header word carries sign+dscale+weight,
 * the digit array extends from n_data. Used when weight/dscale fit inside
 * 7 and 6 bits respectively (the common case).
 */
struct numeric_short {
    std::uint16_t n_header;                      /* Sign + display scale + weight */
    numeric_digit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Digits */
};

/**
 * Long form of an on-disk Numeric. The header carries sign and scale, while weight is
 * stored in a second 16-bit field before the digit array.
 */
struct numeric_long {
    std::uint16_t n_sign_dscale;                 /* Sign + display scale */
    std::int16_t n_weight;                       /* Weight of 1st digit  */
    numeric_digit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Digits */
};

/**
 * Union covering both short and long on-disk forms plus a raw header view.
 */
union numeric_choice {
    std::uint16_t n_header;       /* Header word */
    struct numeric_long n_long;   /* Long form (4-byte header) */
    struct numeric_short n_short; /* Short form (2-byte header) */
};

/**
 * On-disk Numeric: varlena header followed by the representation union.
 * PostgreSQL forward-declares this struct in utils/numeric.h. This definition
 * matches the server representation consumed by the adapter.
 */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL numeric ABI type name
struct NumericData {
    std::int32_t vl_len_;        /* varlena header (do not touch directly!) */
    union numeric_choice choice; /* choice of format */
};

static_assert(PG_VERSION_NUM >= 170000);
static_assert(std::is_standard_layout_v<numeric_short>);
static_assert(std::is_standard_layout_v<numeric_long>);
static_assert(std::is_standard_layout_v<NumericData>);
static_assert(offsetof(numeric_short, n_data) == sizeof(std::uint16_t));
static_assert(offsetof(numeric_long, n_data) == sizeof(std::uint16_t) + sizeof(std::int16_t));
static_assert(offsetof(NumericData, choice) == VARHDRSZ);

namespace {

constexpr std::uint16_t numeric_sign_mask = 0xC000;
constexpr std::uint16_t numeric_positive = 0x0000;
constexpr std::uint16_t numeric_negative = 0x4000;
constexpr std::uint16_t numeric_short_marker = 0x8000;
constexpr std::uint16_t numeric_special = 0xC000;

constexpr int numeric_header_size_long = VARHDRSZ + sizeof(std::uint16_t) + sizeof(std::int16_t);
constexpr int numeric_header_size_short = VARHDRSZ + sizeof(std::uint16_t);

constexpr std::uint16_t numeric_nan = 0xC000;
constexpr std::uint16_t numeric_positive_infinity = 0xD000;
constexpr std::uint16_t numeric_negative_infinity = 0xF000;

constexpr std::uint16_t numeric_short_sign_mask = 0x2000;
constexpr std::uint16_t numeric_short_scale_mask = 0x1F80;
constexpr int numeric_short_scale_shift = 7;
constexpr std::uint16_t numeric_short_scale_maximum =
    numeric_short_scale_mask >> numeric_short_scale_shift;
constexpr std::uint16_t numeric_short_weight_sign_mask = 0x0040;
constexpr std::uint16_t numeric_short_weight_mask = 0x003F;
constexpr int numeric_short_weight_maximum = numeric_short_weight_mask;
constexpr int numeric_short_weight_minimum = -(numeric_short_weight_mask + 1);

constexpr std::uint16_t numeric_scale_mask = 0x3FFF;

/**
 * In-memory working format for arithmetic. The digit buffer is owned by the
 * variable. `digits` points inside it so there is
 * room for carry-out at the top.
 */
struct numeric_variable {
    int digit_count;
    int weight;
    int sign;
    int display_scale;
    numeric_digit* buffer;
    numeric_digit* digits;
};

} // namespace

export module bioseq.postgres.vectorization.types.numeric;

import bioseq.common.logging;
import bioseq.postgres.support;

namespace {

/**
 * Numeric field accessors mirror the server representation consumed by this
 * PostgreSQL-major-specific adapter. numeric_digit_count is non-constexpr
 * because it reads the runtime varlena header through VARSIZE.
 */
[[gnu::always_inline]] constexpr bool numeric_header_is_short(const NumericData* numeric) noexcept {
    return (numeric->choice.n_header & numeric_short_marker) != 0;
}

/** Returns the varlena header size for the active Numeric representation. */
[[gnu::always_inline]] constexpr int numeric_header_size(const NumericData* numeric) noexcept {
    return VARHDRSZ + static_cast<int>(sizeof(std::uint16_t)) +
           (numeric_header_is_short(numeric) ? 0 : static_cast<int>(sizeof(std::int16_t)));
}

/** Returns the display scale encoded by a finite Numeric value. */
[[gnu::always_inline]] constexpr int numeric_display_scale(const NumericData* numeric) noexcept {
    return numeric_header_is_short(numeric)
               ? ((numeric->choice.n_short.n_header & numeric_short_scale_mask) >>
                  numeric_short_scale_shift)
               : (numeric->choice.n_long.n_sign_dscale & numeric_scale_mask);
}

/** Returns the base-10000 weight encoded by a finite Numeric value. */
[[gnu::always_inline]] constexpr int numeric_weight(const NumericData* numeric) noexcept {
    if (numeric_header_is_short(numeric)) {
        const std::uint16_t header = numeric->choice.n_short.n_header;
        return ((header & numeric_short_weight_sign_mask)
                    ? ~static_cast<int>(numeric_short_weight_mask)
                    : 0) |
               (header & numeric_short_weight_mask);
    }
    return numeric->choice.n_long.n_weight;
}

/** Returns the immutable digit range stored after the Numeric header. */
[[gnu::always_inline]] constexpr const numeric_digit*
numeric_digits(const NumericData* numeric) noexcept {
    return numeric_header_is_short(numeric) ? numeric->choice.n_short.n_data
                                            : numeric->choice.n_long.n_data;
}

/** Returns the writable digit range stored after the Numeric header. */
[[gnu::always_inline]] constexpr numeric_digit* numeric_digits(NumericData* numeric) noexcept {
    return numeric_header_is_short(numeric) ? numeric->choice.n_short.n_data
                                            : numeric->choice.n_long.n_data;
}

/** Returns the count of base-10000 digits in one Numeric value. */
[[gnu::always_inline]] inline int numeric_digit_count(const NumericData* numeric) noexcept {
    return static_cast<int>((VARSIZE(numeric) - numeric_header_size(numeric)) /
                            sizeof(numeric_digit));
}

/** Tests whether scale and weight fit PostgreSQL's short Numeric header. */
[[gnu::always_inline]] constexpr bool numeric_can_be_short(int scale, int weight) noexcept {
    return scale <= numeric_short_scale_maximum && weight <= numeric_short_weight_maximum &&
           weight >= numeric_short_weight_minimum;
}

/**
 * Allocates a fresh digit buffer with one spare entry so carry-out
 * can be stored without reallocation. Any prior buffer is freed.
 */
void allocate_numeric_digits(numeric_variable* variable, int digit_count) {
    if (variable->buffer != nullptr)
        pfree(variable->buffer);
    variable->buffer =
        bioseq::postgres::support::allocate_zeroed_array<numeric_digit>(digit_count + 1);
    variable->digits = variable->buffer + 1;
    variable->digit_count = digit_count;
}

/**
 * Releases the digit buffer and leaves the variable in a NaN-shaped
 * unusable state.
 */
void release_numeric_digits(numeric_variable* variable) {
    if (variable->buffer != nullptr)
        pfree(variable->buffer);
    variable->buffer = nullptr;
    variable->digits = nullptr;
    variable->sign = numeric_nan;
}

/** Releases a temporary numeric digit buffer at the C++ scope boundary. */
class numeric_variable_owner final {
  public:
    numeric_variable_owner() noexcept = default;

    ~numeric_variable_owner() noexcept {
        release_numeric_digits(&value);
    }

    numeric_variable_owner(const numeric_variable_owner&) = delete;
    numeric_variable_owner& operator=(const numeric_variable_owner&) = delete;

    /** Returns the owned working representation. */
    [[nodiscard]] numeric_variable& get() noexcept {
        return value;
    }

  private:
    numeric_variable value{};
};

/**
 * Fills a numeric working variable from a signed 128-bit
 * integer. Zero is special-cased so packing emits the canonical
 * "zero with weight 0 positive" form.
 */
void assign_int128_to_numeric_variable(int128 value, numeric_variable* variable) {
    allocate_numeric_digits(variable, 40 / decimal_digits_per_numeric_digit);
    uint128 unsigned_value;
    if (value < 0) {
        variable->sign = numeric_negative;
        unsigned_value = static_cast<uint128>(-(value + 1));
        ++unsigned_value;
    } else {
        variable->sign = numeric_positive;
        unsigned_value = value;
    }
    variable->display_scale = 0;
    if (value == 0) {
        variable->digit_count = 0;
        variable->weight = 0;
        return;
    }
    numeric_digit* digit_ptr = variable->digits + variable->digit_count;
    int digit_count = 0;
    do {
        --digit_ptr;
        ++digit_count;
        const uint128 next_value = unsigned_value / numeric_digit_base;
        *digit_ptr = static_cast<numeric_digit>(unsigned_value - next_value * numeric_digit_base);
        unsigned_value = next_value;
    } while (unsigned_value);
    variable->digits = digit_ptr;
    variable->digit_count = digit_count;
    variable->weight = digit_count - 1;
}

/**
 * Packs a numeric working variable into the on-disk Numeric format. NaN /
 * Infinity emit the minimal short header. Finite values prefer short form
 * when display scale and weight fit.
 */
Numeric pack_numeric(const numeric_variable* variable) {
    Numeric packed_numeric;
    numeric_digit* digits = variable->digits;
    int weight = variable->weight;
    int sign = variable->sign;
    int trimmed_digit_count;
    Size total_length;

    if ((sign & numeric_sign_mask) == numeric_special) {
        if (!(sign == numeric_nan || sign == numeric_positive_infinity ||
              sign == numeric_negative_infinity))
            bioseq_log<logging_level::error, logging_target::postgres>(
                "invalid numeric sign value 0x{:x}", sign);

        packed_numeric = bioseq::postgres::support::allocate_zeroed_storage<NumericData>(
            numeric_header_size_short);
        SET_VARSIZE(packed_numeric, numeric_header_size_short);
        packed_numeric->choice.n_header = sign;
        return packed_numeric;
    }

    trimmed_digit_count = variable->digit_count;

    while (trimmed_digit_count > 0 && *digits == 0) {
        ++digits;
        --weight;
        --trimmed_digit_count;
    }
    while (trimmed_digit_count > 0 && digits[trimmed_digit_count - 1] == 0)
        --trimmed_digit_count;

    if (trimmed_digit_count == 0) {
        weight = 0;
        sign = numeric_positive;
    }

    if (numeric_can_be_short(variable->display_scale, weight)) {
        total_length = numeric_header_size_short + trimmed_digit_count * sizeof(numeric_digit);
        packed_numeric =
            bioseq::postgres::support::allocate_zeroed_storage<NumericData>(total_length);
        SET_VARSIZE(packed_numeric, total_length);
        packed_numeric->choice.n_short.n_header =
            (sign == numeric_negative ? (numeric_short_marker | numeric_short_sign_mask)
                                      : numeric_short_marker) |
            (variable->display_scale << numeric_short_scale_shift) |
            (weight < 0 ? numeric_short_weight_sign_mask : 0) |
            (weight & numeric_short_weight_mask);
    } else {
        total_length = numeric_header_size_long + trimmed_digit_count * sizeof(numeric_digit);
        packed_numeric =
            bioseq::postgres::support::allocate_zeroed_storage<NumericData>(total_length);
        SET_VARSIZE(packed_numeric, total_length);
        packed_numeric->choice.n_long.n_sign_dscale =
            sign | (variable->display_scale & numeric_scale_mask);
        packed_numeric->choice.n_long.n_weight = weight;
    }

    Assert(numeric_digit_count(packed_numeric) == trimmed_digit_count);
    if (trimmed_digit_count > 0)
        std::memcpy(
            numeric_digits(packed_numeric), digits, trimmed_digit_count * sizeof(numeric_digit));

    if (numeric_weight(packed_numeric) != weight ||
        numeric_display_scale(packed_numeric) != variable->display_scale) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE)
            .with_message("value overflows numeric format");
    }

    return packed_numeric;
}

} // namespace

/** Converts a signed 128-bit integer to an owned PostgreSQL Numeric value. */
export Numeric int128_to_numeric(int128 value) {
    numeric_variable_owner working_variable;

    assign_int128_to_numeric_variable(value, &working_variable.get());

    return pack_numeric(&working_variable.get());
}
