/**
 * @file
 * Converts signed 128-bit aggregate values to PostgreSQL Numeric storage.
 */

/*
 * Numeric conversion code adapted from PostgreSQL for vector BIGINT SUM and AVG.
 * It contains only the parts needed to turn a signed 128-bit integer into Numeric.
 * The NumericData definition stays in the global module fragment to match
 * PostgreSQL's forward declaration of that type.
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
 * PostgreSQL's compact Numeric format. One header word stores the sign, display
 * scale and weight, followed by base-10000 digits. The display scale is the
 * number of decimal places, and the weight is the exponent of the first digit.
 */
struct numeric_short {
    std::uint16_t n_header;                      /* Sign + display scale + weight */
    numeric_digit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Base-10000 digits. */
};

/**
 * Numeric format for values whose scale or weight does not fit the compact
 * header. The first word stores sign and scale, and the second stores weight.
 */
struct numeric_long {
    std::uint16_t n_sign_dscale;                 /* Sign + display scale */
    std::int16_t n_weight;                       /* Base-10000 exponent of the first digit. */
    numeric_digit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Base-10000 digits. */
};

/** Access the Numeric header or the fields of its short or long format. */
union numeric_choice {
    std::uint16_t n_header;       /* Header word */
    struct numeric_long n_long;   /* Long form (4-byte header) */
    struct numeric_short n_short; /* Short form (2-byte header) */
};

/**
 * PostgreSQL Numeric storage: a byte-length header followed by the number.
 * This definition completes the NumericData declaration in utils/numeric.h
 * and must match the server's layout.
 */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL numeric ABI type name
struct NumericData {
    std::int32_t vl_len_;        /* Byte-length header. Access it through PostgreSQL varlena helpers. */
    union numeric_choice choice; /* Numeric header and digits in the selected format. */
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
 * Temporary base-10000 digits used while building a Numeric value.
 * buffer owns the allocation. digits points inside it, leaving room before
 * the first digit for a carry.
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

/** Test whether Numeric uses the two-byte numeric header. */
[[gnu::always_inline]] constexpr bool numeric_header_is_short(const NumericData* numeric) noexcept {
    return (numeric->choice.n_header & numeric_short_marker) != 0;
}

/** Return the total header size, including the varlena byte-length header. */
[[gnu::always_inline]] constexpr int numeric_header_size(const NumericData* numeric) noexcept {
    return VARHDRSZ + static_cast<int>(sizeof(std::uint16_t)) +
           (numeric_header_is_short(numeric) ? 0 : static_cast<int>(sizeof(std::int16_t)));
}

/** Return the number of decimal places stored for a finite Numeric value. */
[[gnu::always_inline]] constexpr int numeric_display_scale(const NumericData* numeric) noexcept {
    return numeric_header_is_short(numeric)
               ? ((numeric->choice.n_short.n_header & numeric_short_scale_mask) >>
                  numeric_short_scale_shift)
               : (numeric->choice.n_long.n_sign_dscale & numeric_scale_mask);
}

/** Return the base-10000 exponent of a finite value's first digit. */
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

/** Return a read-only pointer to the Numeric digits. */
[[gnu::always_inline]] constexpr const numeric_digit*
numeric_digits(const NumericData* numeric) noexcept {
    return numeric_header_is_short(numeric) ? numeric->choice.n_short.n_data
                                            : numeric->choice.n_long.n_data;
}

/** Return a writable pointer to the Numeric digits. */
[[gnu::always_inline]] constexpr numeric_digit* numeric_digits(NumericData* numeric) noexcept {
    return numeric_header_is_short(numeric) ? numeric->choice.n_short.n_data
                                            : numeric->choice.n_long.n_data;
}

/** Returns the count of base-10000 digits in one Numeric value. */
[[gnu::always_inline]] inline int numeric_digit_count(const NumericData* numeric) noexcept {
    return static_cast<int>((bioseq::postgres::support::varlena_4byte_size(numeric) - numeric_header_size(numeric)) /
                            sizeof(numeric_digit));
}

/** Tests whether scale and weight fit PostgreSQL's short Numeric header. */
[[gnu::always_inline]] constexpr bool numeric_can_be_short(int scale, int weight) noexcept {
    return scale <= numeric_short_scale_maximum && weight <= numeric_short_weight_maximum &&
           weight >= numeric_short_weight_minimum;
}

/**
 * Replace the digit buffer with a zeroed allocation that has one spare leading
 * digit for a carry. Free the previous buffer, if present.
 */
void allocate_numeric_digits(numeric_variable* variable, int digit_count) {
    if (variable->buffer != nullptr)
        pfree(variable->buffer);
    variable->buffer =
        bioseq::postgres::support::allocate_zeroed_array<numeric_digit>(digit_count + 1);
    variable->digits = variable->buffer + 1;
    variable->digit_count = digit_count;
}

/** Free the digit buffer, clear its pointers and mark the value as NaN. */
void release_numeric_digits(numeric_variable* variable) {
    if (variable->buffer != nullptr)
        pfree(variable->buffer);
    variable->buffer = nullptr;
    variable->digits = nullptr;
    variable->sign = numeric_nan;
}

/** Free a temporary Numeric digit buffer when this object leaves scope. */
class numeric_variable_owner final {
  public:
    numeric_variable_owner() noexcept = default;

    ~numeric_variable_owner() noexcept {
        release_numeric_digits(&value);
    }

    numeric_variable_owner(const numeric_variable_owner&) = delete;
    numeric_variable_owner& operator=(const numeric_variable_owner&) = delete;

    /** Return the temporary value for conversion and packing. */
    [[nodiscard]] numeric_variable& get() noexcept {
        return value;
    }

  private:
    numeric_variable value{};
};

/**
 * Write a signed 128-bit integer as base-10000 digits, with no fractional part.
 * Handle the minimum negative value without overflowing its signed type.
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
 * Allocate a PostgreSQL Numeric from the temporary digits, removing leading
 * and trailing zero digits. Use the smaller header when scale and weight fit.
 * NaN and infinities need only a header. Report an error if the packed fields
 * cannot hold the value.
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

/**
 * Convert a signed 128-bit integer to PostgreSQL Numeric for BIGINT SUM or AVG.
 * Allocate the result in the current memory context and free the working digits.
 */
export Numeric int128_to_numeric(int128 value) {
    numeric_variable_owner working_variable;

    assign_int128_to_numeric_variable(value, &working_variable.get());

    return pack_numeric(&working_variable.get());
}
