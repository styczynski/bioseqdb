/**
 * simdparse: High-speed parser with vector instructions
 * @see https://github.com/hunyadi/simdparse
 *
 * Copyright (c) 2024 Levente Hunyadi
 *
 * This work is licensed under the terms of the MIT license.
 * For a copy, see <https://opensource.org/licenses/MIT>.
 */

#pragma once
#include <array>
#include <limits>
#include <string_view>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cassert>

#if defined(_WIN32) || defined(_WIN64)
#define timegm _mkgmtime
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#endif

 // check if 64-bit SSE2 instructions are available
#if (defined(__GNUC__) && (defined(__x86_64__) || defined(__ppc64__))) || defined(_WIN64)
#define SIMDPARSE_64
#endif

namespace simdparse
{
    namespace detail
    {
        template<typename T>
        static bool parse_range(const std::string_view& str, std::size_t beg, std::size_t end, T& val)
        {
            std::from_chars_result result = std::from_chars(str.data() + beg, str.data() + end, val);
            return result.ec == std::errc{} && result.ptr == str.data() + end;

        }
    }

    /** Represents a date according to the Gregorian calendar. */
    struct date
    {
        constexpr static std::string_view name = "date";

        constexpr date()
        {
        }

        /** Construct a date object from parts. */
        constexpr date(int year, unsigned int month, unsigned int day)
            : year(year)
            , month(month)
            , day(day)
        {
        }

        constexpr bool operator==(const date& op) const
        {
            return year == op.year
                && month == op.month
                && day == op.day
                ;
        }

        constexpr bool operator!=(const date& op) const
        {
            return !(*this == op);
        }

        constexpr bool operator<(const date& op) const
        {
            return compare(op) < 0;
        }

        constexpr bool operator<=(const date& op) const
        {
            return compare(op) <= 0;
        }

        constexpr bool operator>=(const date& op) const
        {
            return compare(op) >= 0;
        }

        constexpr bool operator>(const date& op) const
        {
            return compare(op) > 0;
        }

        constexpr int compare(const date& op) const
        {
            if (year < op.year) {
                return -1;
            } else if (year > op.year) {
                return 1;
            }

            if (month < op.month) {
                return -1;
            } else if (month > op.month) {
                return 1;
            }

            if (day < op.day) {
                return -1;
            } else if (day > op.day) {
                return 1;
            }

            return 0;
        }

#if defined(__AVX2__)
    private:
        /** Parses an RFC 3339 date string with SIMD instructions. */
        bool parse_date(const std::string_view& str)
        {
            alignas(__m128i) std::array<char, 16> buf;
            std::memcpy(buf.data(), str.data(), str.size());
            const __m128i characters = _mm_load_si128(reinterpret_cast<const __m128i*>(buf.data()));

            // validate a date string `YYYY-MM-DD`
            const __m128i lower_bound = _mm_setr_epi8(
                48, 48, 48, 48, // year; 48 = ASCII '0'
                45,             // ASCII '-'
                48, 48,         // month
                45,             // ASCII '-'
                48, 48,         // day
                -128, -128, -128, -128, -128, -128  // don't care
            );
            const __m128i upper_bound = _mm_setr_epi8(
                57, 57, 57, 57, // year; 57 = ASCII '9'
                45,             // ASCII '-'
                49, 57,         // month
                45,             // ASCII '-'
                51, 57,         // day
                127, 127, 127, 127, 127, 127  // don't care
            );

            const __m128i too_low = _mm_cmpgt_epi8(lower_bound, characters);
            const __m128i too_high = _mm_cmpgt_epi8(characters, upper_bound);
            const __m128i out_of_bounds = _mm_or_si128(too_low, too_high);
            if (_mm_movemask_epi8(out_of_bounds)) {
                return false;
            }

            // convert ASCII characters into digit value (offset from character `0`)
            const __m128i ascii_digit_mask = _mm_setr_epi8(15, 15, 15, 15, 0, 15, 15, 0, 15, 15, 0, 0, 0, 0, 0, 0);
            const __m128i spread_integers = _mm_and_si128(characters, ascii_digit_mask);

            // group spread digits `YYYY-MM-DD------` into packed digits `YYYYMMDD--------`
            const __m128i mask = _mm_setr_epi8(
                0, 1, 2, 3,  // year
                5, 6,        // month
                8, 9,        // day
                -1, -1, -1, -1, -1, -1, -1, -1
            );
            const __m128i grouped_integers = _mm_shuffle_epi8(spread_integers, mask);

            // extract values
            union {
                char c[8];
                std::int64_t i;
            } value;

#if defined(SIMDPARSE_64)
            // 64-bit SSE2 instruction
            value.i = _mm_cvtsi128_si64(grouped_integers);
#else
            // equivalent 32-bit SSE2 instruction
            _mm_storeu_si64(value.c, grouped_integers);
#endif

            year = 1000 * value.c[0] + 100 * value.c[1] + 10 * value.c[2] + value.c[3];
            month = 10 * value.c[4] + value.c[5];
            day = 10 * value.c[6] + value.c[7];
            return true;
        }
#else
    private:
        bool parse_date(const std::string_view& str)
        {
            using detail::parse_range;

            // 1984-10-24
            return parse_range(str, 0, 4, year)
                && parse_range(str, 5, 7, month) && month <= 12
                && parse_range(str, 8, 10, day) && day <= 31
                ;
        }
#endif
    public:
        bool parse(const std::string_view& str)
        {
            if (str.size() != 10) {
                return false;
            }

            return parse_date(str);
        }

    public:
        int year = 0;
        unsigned int month = 0;
        unsigned int day = 0;
    };

    struct tzoffset
    {
        struct east_t {};
        constexpr inline static east_t east = east_t();
        struct west_t {};
        constexpr inline static west_t west = west_t();

        constexpr tzoffset()
        {
        }

        constexpr tzoffset(east_t, unsigned int hour, unsigned int minute)
        {
            _value = static_cast<int>(60 * hour + minute);
        }

        constexpr tzoffset(west_t, unsigned int hour, unsigned int minute)
        {
            _value = -static_cast<int>(60 * hour + minute);
        }

        constexpr bool operator==(const tzoffset& op) const
        {
            return _value == op._value;
        }

        constexpr bool operator!=(const tzoffset& op) const
        {
            return _value != op._value;
        }

        constexpr bool operator<(const tzoffset& op) const
        {
            return _value < op._value;
        }

        constexpr bool operator<=(const tzoffset& op) const
        {
            return _value <= op._value;
        }

        constexpr bool operator>=(const tzoffset& op) const
        {
            return _value >= op._value;
        }

        constexpr bool operator>(const tzoffset& op) const
        {
            return _value > op._value;
        }

        constexpr int minutes() const
        {
            return _value;
        }

        void assign(int minutes)
        {
            _value = minutes;
        }

        bool parse(const std::string_view& str)
        {
            if (str.size() != 6) {
                return false;
            }

            using detail::parse_range;

            // -01:30
            unsigned int hour;
            unsigned int minute;
            if (!parse_range(str, 1, 3, hour) || str[3] != ':' || !parse_range(str, 4, 6, minute)) {
                return false;
            }

            int sign = (str[0] == '+') - (str[0] == '-');
            _value = sign * (60 * hour + minute);
            return sign != 0 && minute < 60;
        }

    private:
        int _value = 0;
    };

    /** Date and time, with date according to the Gregorian calendar, and time as per the specified time zone. */
    struct datetime
    {
        constexpr static std::string_view name = "date-time";

        constexpr datetime()
        {
        }

        /** Construct a date-time object from parts. */
        constexpr datetime(
            int year,
            unsigned int month,
            unsigned int day,
            unsigned int hour,
            unsigned int minute,
            unsigned int second,
            tzoffset offset = tzoffset()
        )
            : year(year)
            , month(month)
            , day(day)
            , hour(hour)
            , minute(minute)
            , second(second)
            , offset(offset)
        {
        }

        /** Construct a date-time object from parts. */
        constexpr datetime(
            int year,
            unsigned int month,
            unsigned int day,
            unsigned int hour,
            unsigned int minute,
            unsigned int second,
            unsigned long nanosecond,
            tzoffset offset = tzoffset()
        )
            : year(year)
            , month(month)
            , day(day)
            , hour(hour)
            , minute(minute)
            , second(second)
            , nanosecond(nanosecond)
            , offset(offset)
        {
        }

        constexpr bool operator==(const datetime& op) const
        {
            return year == op.year
                && month == op.month
                && day == op.day
                && hour == op.hour
                && minute == op.minute
                && second == op.second
                && nanosecond == op.nanosecond
                && offset == op.offset
                ;
        }

        constexpr bool operator!=(const datetime& op) const
        {
            return !(*this == op);
        }

        constexpr bool operator<(const datetime& op) const
        {
            return compare(op) < 0;
        }

        constexpr bool operator<=(const datetime& op) const
        {
            return compare(op) <= 0;
        }

        constexpr bool operator>=(const datetime& op) const
        {
            return compare(op) >= 0;
        }

        constexpr bool operator>(const datetime& op) const
        {
            return compare(op) > 0;
        }

        constexpr int compare(const datetime& op) const
        {
            if (year < op.year) {
                return -1;
            } else if (year > op.year) {
                return 1;
            }

            if (month < op.month) {
                return -1;
            } else if (month > op.month) {
                return 1;
            }

            if (day < op.day) {
                return -1;
            } else if (day > op.day) {
                return 1;
            }

            if (hour < op.hour) {
                return -1;
            } else if (hour > op.hour) {
                return 1;
            }

            if (minute < op.minute) {
                return -1;
            } else if (minute > op.minute) {
                return 1;
            }

            if (second < op.second) {
                return -1;
            } else if (second > op.second) {
                return 1;
            }

            if (nanosecond < op.nanosecond) {
                return -1;
            } else if (nanosecond > op.nanosecond) {
                return 1;
            }

            if (offset < op.offset) {
                return -1;
            } else if (offset > op.offset) {
                return 1;
            }

            return 0;
        }

        static constexpr datetime zero()
        {
            return datetime();
        }

        static constexpr datetime max()
        {
            return datetime(9999, 12, 31, 23, 59, 59, 999999999);
        }

        /** Parses a date-time string with an optional time zone offset. */
        bool parse(const char* beg, const char* end)
        {
            return parse(std::string_view(beg, end - beg));
        }

        /** Parses a date-time string with an optional time zone offset. */
        bool parse(const char* beg, std::size_t siz)
        {
            return parse(std::string_view(beg, siz));
        }

        /** Parses a date-time string with an optional time zone offset. */
        bool parse(const std::string_view& str)
        {
            if (str.size() < 19 || str.size() > 35) {
                return false;
            }

            if (str.back() == 'Z') {
                // 1984-10-24 23:59:59.123456789Z
                // 1984-10-24 23:59:59.123456Z
                // 1984-10-24 23:59:59.123Z
                // 1984-10-24 23:59:59Z
                if (!parse_naive_date_time(str.substr(0, str.size() - 1))) {
                    return false;
                }
                offset = tzoffset();
                return true;
            }

            char offset_sign = str[str.size() - 6];
            if (offset_sign == '+' || offset_sign == '-') {
                // 1984-10-24 23:59:59.123456789+00:00
                // 1984-10-24 23:59:59.123456+00:00
                // 1984-10-24 23:59:59.123+00:00
                // 1984-10-24 23:59:59+00:00
                if (!parse_naive_date_time(str.substr(0, str.size() - 6))) {
                    return false;
                }
                if (!offset.parse(str.substr(str.size() - 6, 6))) {
                    return false;
                }
                return true;
            }

            if (std::memcmp(" UTC", str.data() + str.size() - 4, 4) == 0) {
                // 1984-10-24 23:59:59 UTC
                if (!parse_naive_date_time(str.substr(0, str.size() - 4))) {
                    return false;
                }
                offset = tzoffset();
                return true;
            } else {
                // 1984-10-24 23:59:59.123456789
                // 1984-10-24 23:59:59.123456
                // 1984-10-24 23:59:59.123
                // 1984-10-24 23:59:59
                if (!parse_naive_date_time(str)) {
                    return false;
                }
                offset = tzoffset();
                return true;
            }
        }

#if defined(__AVX2__)
    private:
        /**
         * Parses an RFC 3339 date-time string with SIMD instructions.
         *
         * @see https://movermeyer.com/2023-01-04-rfc-3339-simd/
         */
        bool parse_date_time(const std::string_view& str)
        {
            assert(str.size() == 19);

            const __m128i characters = _mm_loadu_si128(reinterpret_cast<const __m128i*>(str.data()));

            // validate a 16-byte partial date-time string `YYYY-MM-DDThh:mm`
            const __m128i lower_bound = _mm_setr_epi8(
                48, 48, 48, 48, // year; 48 = ASCII '0'
                45,             // ASCII '-'
                48, 48,         // month
                45,             // ASCII '-'
                48, 48,         // day
                32,             // ASCII ' '
                48, 48,         // hour
                58,             // ASCII ':'
                48, 48          // minute
            );
            const __m128i upper_bound = _mm_setr_epi8(
                57, 57, 57, 57, // year; 57 = ASCII '9'
                45,             // ASCII '-'
                49, 57,         // month
                45,             // ASCII '-'
                51, 57,         // day
                84,             // ASCII 'T'
                50, 57,         // hour
                58,             // ASCII ':'
                53, 57          // minute
            );

            const __m128i too_low = _mm_cmpgt_epi8(lower_bound, characters);
            const __m128i too_high = _mm_cmpgt_epi8(characters, upper_bound);
            const __m128i out_of_bounds = _mm_or_si128(too_low, too_high);
            if (_mm_movemask_epi8(out_of_bounds)) {
                return false;
            }

            // convert ASCII characters into digit value (offset from character `0`)
            const __m128i ascii_digit_mask = _mm_setr_epi8(15, 15, 15, 15, 0, 15, 15, 0, 15, 15, 0, 15, 15, 0, 15, 15); // 15 = 0x0F
            const __m128i spread_integers = _mm_and_si128(characters, ascii_digit_mask);

            // group spread digits `YYYY-MM-DD hh:mm` into packed digits `YYYYMMDDhhmm----`
            const __m128i mask = _mm_setr_epi8(
                0, 1, 2, 3,  // year
                5, 6,        // month
                8, 9,        // day
                11, 12,      // hour
                14, 15,      // minute
                -1, -1, -1, -1
            );
            const __m128i packed_integers = _mm_shuffle_epi8(spread_integers, mask);

            // fuse neighboring digits into a single value
            const __m128i weights = _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 0, 0, 0, 0);
            const __m128i values = _mm_maddubs_epi16(packed_integers, weights);

            // extract values
            alignas(__m128i) std::array<std::uint16_t, 8> result;
            _mm_store_si128(reinterpret_cast<__m128i*>(result.data()), values);

            year = (result[0] * 100) + result[1];
            month = result[2];
            day = result[3];
            hour = result[4];
            minute = result[5];

            return detail::parse_range(str, 17, 19, second) && second < 60;
        }

        /** Parses an RFC 3339 date-time string with a fractional part using SIMD instructions. */
        bool parse_date_time_fractional(const std::string_view& str)
        {
            assert(str.size() <= 29);

            alignas(__m256i) std::array<char, 32> buf;
            std::memcpy(buf.data(), str.data(), str.size());
            std::memset(buf.data() + str.size(), '0', 32 - str.size());

            const __m256i characters = _mm256_load_si256(reinterpret_cast<const __m256i*>(buf.data()));

            // validate a 32-byte partial date-time string `YYYY-MM-DDThh:mm:ss.fffffffff---`
            const __m256i lower_bound = _mm256_setr_epi8(
                48, 48, 48, 48, // year; 48 = ASCII '0'
                45,             // ASCII '-'
                48, 48,         // month
                45,             // ASCII '-'
                48, 48,         // day
                32,             // ASCII ' '
                48, 48,         // hour
                58,             // ASCII ':'
                48, 48,         // minute
                58,             // ASCII ':'
                48, 48,         // second
                46,             // ASCII '.'
                48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48
            );
            const __m256i upper_bound = _mm256_setr_epi8(
                57, 57, 57, 57, // year; 57 = ASCII '9'
                45,             // ASCII '-'
                49, 57,         // month
                45,             // ASCII '-'
                51, 57,         // day
                84,             // ASCII 'T'
                50, 57,         // hour
                58,             // ASCII ':'
                53, 57,         // minute
                58,             // ASCII ':'
                53, 57,         // second
                46,             // ASCII '.'
                57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57
            );

            const __m256i too_low = _mm256_cmpgt_epi8(lower_bound, characters);
            const __m256i too_high = _mm256_cmpgt_epi8(characters, upper_bound);
            const __m256i out_of_bounds = _mm256_or_si256(too_low, too_high);
            if (_mm256_movemask_epi8(out_of_bounds)) {
                return false;
            }

            // convert ASCII characters into digit value (offset from character `0`)
            const __m256i ascii_digit_mask = _mm256_setr_epi8(
                15, 15, 15, 15,  // year
                0,
                15, 15,          // month
                0,
                15, 15,          // day
                0,
                15, 15,          // hour
                0,
                15, 15,          // minute
                0,
                15, 15,          // second
                0,
                15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
            );
            const __m256i spread_integers = _mm256_and_si256(characters, ascii_digit_mask);

            // group spread digits `YYYY-MM-DD hh:mm:ss.fffffffff---` into packed digits `YYYYMMDDhhmm----ss-fff-fff-fff--`
            const __m256i mask = _mm256_setr_epi8(
                0, 1, 2, 3,  // year
                5, 6,        // month
                8, 9,        // day
                11, 12,      // hour
                14, 15,      // minute
                -1, -1, -1, -1,
                1, 2,        // second
                -1,
                4, 5, 6,     // millisecond range
                -1,
                7, 8, 9,     // microsecond range
                -1,
                10, 11, 12,  // nanosecond range
                -1, -1
            );
            const __m256i packed_integers = _mm256_shuffle_epi8(spread_integers, mask);

            // fuse neighboring digits into a single value
            const __m256i weights = _mm256_setr_epi8(
                10, 1, 10, 1,   // year
                10, 1,          // month
                10, 1,          // day
                10, 1,          // hour
                10, 1,          // minute
                0, 0, 0, 0,
                10, 1,          // second
                0, 100, 10, 1,  // millisecond range
                0, 100, 10, 1,  // microsecond range
                0, 100, 10, 1,  // nanosecond range
                0, 0
            );
            const __m256i values = _mm256_maddubs_epi16(packed_integers, weights);

            // extract values
            alignas(__m256i) std::array<std::int16_t, 16> result;
            _mm256_store_si256(reinterpret_cast<__m256i*>(result.data()), values);

            year = 100 * result[0] + result[1];
            month = result[2];
            day = result[3];
            hour = result[4];
            minute = result[5];
            second = result[8];
            unsigned int milli = result[9] + result[10];
            unsigned int micro = result[11] + result[12];
            unsigned int nano = result[13] + result[14];
            nanosecond = 1'000'000ull * milli + 1'000ull * micro + nano;
            return true;
        }
#else
    private:
        /** Parses an RFC 3339 date-time string. */
        bool parse_date_time(const std::string_view& str)
        {
            assert(str.size() == 19);

            using detail::parse_range;

            // 1984-10-24 23:59:59
            return parse_range(str, 0, 4, year)
                && str[4] == '-'
                && parse_range(str, 5, 7, month) && month <= 12
                && str[7] == '-'
                && parse_range(str, 8, 10, day) && day <= 31
                && (str[10] == 'T' || str[10] == ' ')
                && parse_range(str, 11, 13, hour) && hour < 24
                && str[13] == ':'
                && parse_range(str, 14, 16, minute) && minute < 60
                && str[16] == ':'
                && parse_range(str, 17, 19, second) && second < 60
                ;
        }

        /** Parses an RFC 3339 date-time string with a fractional part. */
        bool parse_date_time_fractional(const std::string_view& str)
        {
            assert(str.size() <= 29);

            return parse_date_time(str.substr(0, 19))
                && str[19] == '.'
                && parse_fractional(str.substr(20))
                ;
        }
#endif

        /** Parses an RFC 3339 date-time string without time zone offset. */
        bool parse_naive_date_time(const std::string_view& str)
        {
            if (str.size() > 29 || str.size() < 19) {
                return false;
            } else if (str.size() > 19) {
                return parse_date_time_fractional(str);
            } else {
                return parse_date_time(str);
            }
        }

        /** Parses the fractional part of a date-time string. */
        bool parse_fractional(const std::string_view& str)
        {
            assert(str.size() <= 9);

            constexpr static std::array<unsigned long, 9> powers = {
                1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000
            };
            unsigned long fractional;
            if (!detail::parse_range(str, 0, str.size(), fractional)) {
                return false;
            }
            unsigned long scale = powers[9 - str.size()];
            nanosecond = scale * fractional;
            return true;
        }

    public:
        int year = 0;
        unsigned int month = 0;
        unsigned int day = 0;
        unsigned int hour = 0;
        unsigned int minute = 0;
        unsigned int second = 0;
        unsigned long nanosecond = 0;
        tzoffset offset;
    };

    /** A UTC timestamp with microsecond precision. */
    struct microtime
    {
        constexpr static std::string_view name = "timestamp with microsecond precision";

        constexpr microtime()
        {
        }

        constexpr explicit microtime(std::int64_t ms)
            : _value(ms)
        {
        }

        /** Construct a timestamp from parts. */
        microtime(
            int year,
            unsigned int month,
            unsigned int day,
            unsigned int hour,
            unsigned int minute,
            unsigned int second,
            tzoffset offset = tzoffset()
        )
        {
            assign(year, month, day, hour, minute, second, 0, offset);
        }

        /** Construct a timestamp from parts. */
        microtime(
            int year,
            unsigned int month,
            unsigned int day,
            unsigned int hour,
            unsigned int minute,
            unsigned int second,
            unsigned long microsecond,
            tzoffset offset = tzoffset()
        )
        {
            assign(year, month, day, hour, minute, second, microsecond, offset);
        }

        static constexpr const std::int64_t UNSET = std::numeric_limits<std::int64_t>::min();

        constexpr bool undefined() const
        {
            return _value == UNSET;
        }

        constexpr bool operator==(const microtime& op) const
        {
            return _value == op._value;
        }

        constexpr bool operator!=(const microtime& op) const
        {
            return _value != op._value;
        }

        constexpr bool operator<(const microtime& op) const
        {
            return _value < op._value;
        }

        constexpr bool operator<=(const microtime& op) const
        {
            return _value <= op._value;
        }

        constexpr bool operator>=(const microtime& op) const
        {
            return _value >= op._value;
        }

        constexpr bool operator>(const microtime& op) const
        {
            return _value > op._value;
        }

        /** Microseconds fractional part of timestamp. */
        constexpr unsigned long microseconds() const
        {
            if (undefined()) {
                return 0;
            }

            auto v = _value > 0 ? _value : -_value;
            return static_cast<unsigned long>(v % 1'000'000);
        }

        /** Date and time component of the timestamp, up to seconds precision. */
        std::time_t as_time() const
        {
            if (undefined()) {
                return static_cast<std::time_t>(-1);  // signals error
            }

            return static_cast<std::time_t>(_value / 1'000'000);
        }

        /** Returns the Gregorian calendar date of this time instant. */
        date as_date() const
        {
            std::time_t timer = as_time();
            std::tm* ts = gmtime(&timer);
            if (ts == nullptr) {
                return date();
            }

            return date(
                ts->tm_year + 1900,
                ts->tm_mon + 1,
                ts->tm_mday
            );
        }

        /** Returns the (Gregorian) date and (UTC) time of this time instant. */
        datetime as_datetime() const
        {
            if (undefined()) {
                return datetime();
            }

            std::time_t timer = as_time();
            std::tm* ts = gmtime(&timer);
            if (ts == nullptr) {
                return datetime();
            }

            return datetime(
                ts->tm_year + 1900,
                ts->tm_mon + 1,
                ts->tm_mday,
                ts->tm_hour,
                ts->tm_min,
                ts->tm_sec,
                1000 * microseconds()
            );
        }

        /** Sets the (Gregorian) date and time (with time zone). */
        void assign(int year, unsigned int month, unsigned int day, unsigned int hour, unsigned int minute, unsigned int second, unsigned long microsecond, const tzoffset& offset)
        {
            // use 400-year periodicity of Gregorian calendar
            int periods = 0;
            while (year < 1900) {
                year += 400;
                ++periods;
            }

            std::tm ts{};
            ts.tm_year = year - 1900;
            ts.tm_mon = month - 1;
            ts.tm_mday = day;
            ts.tm_hour = hour - offset.minutes() / 60;
            ts.tm_min = minute - offset.minutes() % 60;
            ts.tm_sec = second;
            ts.tm_isdst = 0;
            std::time_t t = timegm(&ts);
            if (t != static_cast<std::time_t>(-1)) {
                _value = t;
                _value -= periods * (146'097ll * 24 * 60 * 60);
                _value *= 1'000'000;
                if (_value > 0) {
                    _value += microsecond;
                } else {
                    _value -= microsecond;
                }
            } else {
                _value = UNSET;
            }
        }

        bool parse(const char* beg, const char* end)
        {
            return parse(std::string_view(beg, end - beg));
        }

        bool parse(const char* beg, std::size_t siz)
        {
            return parse(std::string_view(beg, siz));
        }

        bool parse(const std::string_view& str)
        {
            datetime dt;
            if (!dt.parse(str)) {
                return false;
            }
            assign(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.nanosecond / 1'000, dt.offset);
            return true;
        }

        /** Returns microseconds before/after epoch. */
        constexpr std::int64_t value() const
        {
            return _value;
        }

    private:
        /** Microseconds before/after epoch. */
        std::int64_t _value = UNSET;
    };

    namespace detail
    {
        /**
         * True if the character may be an ASCII letter character.
         *
         * Uppercase letters in ASCII have the binary representation of 0b010xxxxx.
         * Lowercase letters have the binary representation of 0b011xxxxx.
         */
        inline constexpr bool maybe_letter(char c)
        {
            return (c & 0b11000000) == 0b01000000;
        }

        /**
         * Compacts ASCII characters of the abbreviated English month name into a numeric value.
         *
         * This function is case insensitive as it takes into account the lowest 5 bits only.
         */
        inline constexpr std::uint16_t month_to_integer(char c1, char c2, char c3)
        {
            return
                static_cast<std::uint16_t>(c1 & 0b00011111) << 10 |
                static_cast<std::uint16_t>(c2 & 0b00011111) << 5 |
                static_cast<std::uint16_t>(c3 & 0b00011111)
                ;
        }

        /**
         * Holds the numeric offset associated with each abbreviated month name.
         *
         * 0 for January, 1 for February, ..., 11 for December, 15 for invalid data.
         */
        constexpr static inline std::uint8_t month_offsets[] = {
            7, 6, 4, 8, 9, 11, 2, 3, 0, 5, 10, 1, 15, 15, 15, 15
        };

        /**
         * Holds the numeric value associated with each abbreviated month name.
         */
        constexpr static inline std::uint16_t month_values[] = {
            month_to_integer('J', 'a', 'n'),
            month_to_integer('F', 'e', 'b'),
            month_to_integer('M', 'a', 'r'),
            month_to_integer('A', 'p', 'r'),
            month_to_integer('M', 'a', 'y'),
            month_to_integer('J', 'u', 'n'),
            month_to_integer('J', 'u', 'l'),
            month_to_integer('A', 'u', 'g'),
            month_to_integer('S', 'e', 'p'),
            month_to_integer('O', 'c', 't'),
            month_to_integer('N', 'o', 'v'),
            month_to_integer('D', 'e', 'c'),
            0, 0, 0, 0
        };
    }

    /**
     * Converts an abbreviated English month name into an ordinal.
     *
     * @param c1 1st character of the abbreviated English month name, e.g. `J` or `O`.
     * @param c2 2nd character of the abbreviated English month name, e.g. `a` or `c`.
     * @param c3 3rd character of the abbreviated English month name, e.g. `n` or `t`.
     * @returns `1` to `12` for `Jan` to `Dec`, respectively, or `0` on parse error.
     */
    constexpr inline unsigned int month_to_ordinal(char c1, char c2, char c3)
    {
        using detail::maybe_letter, detail::month_values;

        // constants for perfect hashing of months into a contiguous array of 12
        constexpr unsigned int k = 68;
        constexpr unsigned int p = 929;

        // calculate the perfect hash value to get the proper index in the array
        std::uint16_t value = detail::month_to_integer(c1, c2, c3);
        unsigned int index = ((k * value) % p) & 0b00001111;

        // check for false positives in the lookup table
        unsigned int offset = detail::month_offsets[index];
        if (maybe_letter(c1) && maybe_letter(c2) && maybe_letter(c3) && value == month_values[offset]) {
            return offset + 1;
        } else {
            return 0;
        }
    }

    /**
     * Converts an abbreviated English month name into an ordinal.
     *
     * @param abbr Abbreviated English month name, e.g. `Jan` or `Oct`.
     * @returns `1` to `12` for `Jan` to `Dec`, respectively, or `0` on parse error.
     */
    inline unsigned int month_to_ordinal(const std::array<char, 3>& abbr)
    {
        return month_to_ordinal(abbr[0], abbr[1], abbr[2]);
    }

    /**
     * Converts an abbreviated English month name into an ordinal.
     *
     * @param abbr Abbreviated English month name, e.g. `Jan` or `Oct`.
     * @returns `1` to `12` for `Jan` to `Dec`, respectively, or `0` on parse error.
     */
    inline unsigned int month_to_ordinal(const std::string_view& abbr)
    {
        if (abbr.size() != 3) {
            return 0;
        } else {
            return month_to_ordinal(abbr[0], abbr[1], abbr[2]);
        }
    }
}
