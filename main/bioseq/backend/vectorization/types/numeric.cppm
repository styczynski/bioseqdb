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

#include "common.h"

#include "columnar/vectorization/types/types.h"

namespace {

constexpr int NBASE = 10000;
constexpr int HALF_NBASE = 5000;
constexpr int DEC_DIGITS = 4;          /* decimal digits per NBASE digit */
constexpr int MUL_GUARD_DIGITS = 2;    /* measured in NBASE digits */
constexpr int DIV_GUARD_DIGITS = 4;

using NumericDigit = int16_t;

}

/*
 * Short form of an on-disk Numeric: header word carries sign+dscale+weight,
 * the digit array extends from n_data. Used when weight/dscale fit inside
 * 7 and 6 bits respectively (the common case).
 */
struct NumericShort
{
	uint16_t n_header;                         /* Sign + display scale + weight */
	NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER];/* Digits */
};

/*
 * Long form of an on-disk Numeric: header carries sign+dscale; weight is
 * stored in a second 16-bit field before the digit array.
 */
struct NumericLong
{
	uint16_t n_sign_dscale;                    /* Sign + display scale */
	int16_t n_weight;                          /* Weight of 1st digit  */
	NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER];/* Digits */
};

/*
 * Union covering both short and long on-disk forms plus a raw header view.
 */
union NumericChoice
{
	uint16_t n_header;                /* Header word */
	struct NumericLong n_long;        /* Long form (4-byte header) */
	struct NumericShort n_short;      /* Short form (2-byte header) */
};

/*
 * On-disk Numeric: varlena header followed by a NumericChoice. PostgreSQL
 * forward-declares this struct in utils/numeric.h; we fill in the body
 * here to match the internal layout used by the server.
 */
struct NumericData
{
	int32_t vl_len_;                  /* varlena header (do not touch directly!) */
	union NumericChoice choice;       /* choice of format */
};

namespace {

constexpr uint16_t NUMERIC_SIGN_MASK = 0xC000;
constexpr uint16_t NUMERIC_POS = 0x0000;
constexpr uint16_t NUMERIC_NEG = 0x4000;
constexpr uint16_t NUMERIC_SHORT = 0x8000;
constexpr uint16_t NUMERIC_SPECIAL = 0xC000;

#define NUMERIC_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_SIGN_MASK)
#define NUMERIC_IS_SHORT(n)		(NUMERIC_FLAGBITS(n) == NUMERIC_SHORT)
#define NUMERIC_IS_SPECIAL(n)	(NUMERIC_FLAGBITS(n) == NUMERIC_SPECIAL)

constexpr int NUMERIC_HDRSZ = VARHDRSZ + sizeof(uint16_t) + sizeof(int16_t);
constexpr int NUMERIC_HDRSZ_SHORT = VARHDRSZ + sizeof(uint16_t);

#define NUMERIC_HEADER_IS_SHORT(n)	(((n)->choice.n_header & 0x8000) != 0)
#define NUMERIC_HEADER_SIZE(n) \
	(VARHDRSZ + sizeof(uint16_t) + \
	 (NUMERIC_HEADER_IS_SHORT(n) ? 0 : sizeof(int16_t)))

constexpr uint16_t NUMERIC_EXT_SIGN_MASK = 0xF000;
constexpr uint16_t NUMERIC_NAN = 0xC000;
constexpr uint16_t NUMERIC_PINF = 0xD000;
constexpr uint16_t NUMERIC_NINF = 0xF000;
constexpr uint16_t NUMERIC_INF_SIGN_MASK = 0x2000;

#define NUMERIC_EXT_FLAGBITS(n)	((n)->choice.n_header & NUMERIC_EXT_SIGN_MASK)

constexpr uint16_t NUMERIC_SHORT_SIGN_MASK = 0x2000;
constexpr uint16_t NUMERIC_SHORT_DSCALE_MASK = 0x1F80;
constexpr int NUMERIC_SHORT_DSCALE_SHIFT = 7;
constexpr uint16_t NUMERIC_SHORT_DSCALE_MAX = NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT;
constexpr uint16_t NUMERIC_SHORT_WEIGHT_SIGN_MASK = 0x0040;
constexpr uint16_t NUMERIC_SHORT_WEIGHT_MASK = 0x003F;
constexpr int NUMERIC_SHORT_WEIGHT_MAX = NUMERIC_SHORT_WEIGHT_MASK;
constexpr int NUMERIC_SHORT_WEIGHT_MIN = -(NUMERIC_SHORT_WEIGHT_MASK + 1);

constexpr uint16_t NUMERIC_DSCALE_MASK = 0x3FFF;
constexpr uint16_t NUMERIC_DSCALE_MAX = NUMERIC_DSCALE_MASK;

#define NUMERIC_DSCALE(n)	(NUMERIC_HEADER_IS_SHORT((n)) ? \
	((n)->choice.n_short.n_header & NUMERIC_SHORT_DSCALE_MASK) \
		>> NUMERIC_SHORT_DSCALE_SHIFT \
	: ((n)->choice.n_long.n_sign_dscale & NUMERIC_DSCALE_MASK))

#define NUMERIC_WEIGHT(n)	(NUMERIC_HEADER_IS_SHORT((n)) ? \
	(((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? \
		~NUMERIC_SHORT_WEIGHT_MASK : 0) \
	 | ((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_MASK)) \
	: ((n)->choice.n_long.n_weight))

/*
 * NumericVar is the in-memory working format for arithmetic. The digit
 * buffer (buf) is owned by the variable; digits points inside it so there's
 * room for carry-out at the top.
 */
struct NumericVar
{
	int ndigits;
	int weight;
	int sign;
	int dscale;
	NumericDigit* buf;
	NumericDigit* digits;
};

}

export module bioseq_vector_types_numeric;

import mlogging;
import pg_utils;

namespace {

#define NUMERIC_DIGITS(num) (NUMERIC_HEADER_IS_SHORT(num) ? \
	(num)->choice.n_short.n_data : (num)->choice.n_long.n_data)
#define NUMERIC_NDIGITS(num) \
	((VARSIZE(num) - NUMERIC_HEADER_SIZE(num)) / sizeof(NumericDigit))
#define NUMERIC_CAN_BE_SHORT(scale,weight) \
	((scale) <= NUMERIC_SHORT_DSCALE_MAX && \
	(weight) <= NUMERIC_SHORT_WEIGHT_MAX && \
	(weight) >= NUMERIC_SHORT_WEIGHT_MIN)

/*
 * alloc_var allocates a fresh digit buffer with one spare entry so carry-out
 * can be stored without reallocation. Any prior buffer is freed.
 */
void
alloc_var(NumericVar* var, int digit_count)
{
	if (var->buf != nullptr)
		pfree(var->buf);
	var->buf = make_array<NumericDigit>(digit_count + 1);
	var->buf[0] = 0;
	var->digits = var->buf + 1;
	var->ndigits = digit_count;
}

/*
 * free_var releases the digit buffer and leaves the var in a NaN-shaped
 * unusable state.
 */
void
free_var(NumericVar* var)
{
	if (var->buf != nullptr)
		pfree(var->buf);
	var->buf = nullptr;
	var->digits = nullptr;
	var->sign = NUMERIC_NAN;
}

/*
 * int128_to_numericvar fills out a NumericVar from a signed 128-bit
 * integer. Zero is special-cased so make_numeric emits the canonical
 * "zero with weight 0 positive" form.
 */
void
int128_to_numericvar(int128 value, NumericVar* var)
{
	alloc_var(var, 40 / DEC_DIGITS);
	uint128 unsigned_value;
	if (value < 0)
	{
		var->sign = NUMERIC_NEG;
		unsigned_value = -value;
	}
	else
	{
		var->sign = NUMERIC_POS;
		unsigned_value = value;
	}
	var->dscale = 0;
	if (value == 0)
	{
		var->ndigits = 0;
		var->weight = 0;
		return;
	}
	NumericDigit* digit_ptr = var->digits + var->ndigits;
	int digit_count = 0;
	do
	{
		digit_ptr--;
		digit_count++;
		uint128 next_value = unsigned_value / NBASE;
		*digit_ptr = unsigned_value - next_value * NBASE;
		unsigned_value = next_value;
	} while (unsigned_value);
	var->digits = digit_ptr;
	var->ndigits = digit_count;
	var->weight = digit_count - 1;
}

/*
 * make_numeric packs a NumericVar into the on-disk Numeric format. NaN /
 * Infinity emit the minimal short header; finite values prefer short form
 * when dscale and weight fit.
 */
Numeric
make_numeric(const NumericVar* var)
{
	Numeric result;
	NumericDigit* digits = var->digits;
	int weight = var->weight;
	int sign = var->sign;
	int trimmed_digit_count;
	Size total_length;

	if ((sign & NUMERIC_SIGN_MASK) == NUMERIC_SPECIAL)
	{
		if (!(sign == NUMERIC_NAN ||
			  sign == NUMERIC_PINF ||
			  sign == NUMERIC_NINF))
			bioseq_log<ERR, PG>( "invalid numeric sign value 0x{:x}", sign);

		result = make1<NumericData>(NUMERIC_HDRSZ_SHORT);
		SET_VARSIZE(result, NUMERIC_HDRSZ_SHORT);
		result->choice.n_header = sign;
		return result;
	}

	trimmed_digit_count = var->ndigits;

	while (trimmed_digit_count > 0 && *digits == 0)
	{
		digits++;
		weight--;
		trimmed_digit_count--;
	}
	while (trimmed_digit_count > 0 && digits[trimmed_digit_count - 1] == 0)
		trimmed_digit_count--;

	if (trimmed_digit_count == 0)
	{
		weight = 0;
		sign = NUMERIC_POS;
	}

	if (NUMERIC_CAN_BE_SHORT(var->dscale, weight))
	{
		total_length = NUMERIC_HDRSZ_SHORT + trimmed_digit_count * sizeof(NumericDigit);
		result = make1<NumericData>(total_length);
		SET_VARSIZE(result, total_length);
		result->choice.n_short.n_header =
			(sign == NUMERIC_NEG ? (NUMERIC_SHORT | NUMERIC_SHORT_SIGN_MASK)
			 : NUMERIC_SHORT)
			| (var->dscale << NUMERIC_SHORT_DSCALE_SHIFT)
			| (weight < 0 ? NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0)
			| (weight & NUMERIC_SHORT_WEIGHT_MASK);
	}
	else
	{
		total_length = NUMERIC_HDRSZ + trimmed_digit_count * sizeof(NumericDigit);
		result = make1<NumericData>(total_length);
		SET_VARSIZE(result, total_length);
		result->choice.n_long.n_sign_dscale =
			sign | (var->dscale & NUMERIC_DSCALE_MASK);
		result->choice.n_long.n_weight = weight;
	}

	Assert(NUMERIC_NDIGITS(result) == trimmed_digit_count);
	if (trimmed_digit_count > 0)
		memcpy(NUMERIC_DIGITS(result), digits, trimmed_digit_count * sizeof(NumericDigit));

	if (NUMERIC_WEIGHT(result) != weight ||
		NUMERIC_DSCALE(result) != var->dscale)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE).msg("value overflows numeric format");
	}

	return result;
}

}

extern "C++" {

/*
 * int128_to_numeric is the public entry point exposed to the vectorised
 * aggregate path. Delegates to int128_to_numericvar + make_numeric and
 * cleans up the working NumericVar on exit.
 */
Numeric int128_to_numeric(int128 value)
{
	NumericVar working_var;
	memset(&working_var, 0, sizeof(NumericVar));

	int128_to_numericvar(value, &working_var);

	Numeric packed_result = make_numeric(&working_var);

	free_var(&working_var);

	return packed_result;
}

}
