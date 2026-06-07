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
#include <string_view>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#elif (defined(__i386__) || defined(__x86_64__))
#include <tmmintrin.h>
#endif

namespace simdparse
{
    struct decimal_integer
    {
        constexpr static std::string_view name = "decimal integer";

        constexpr decimal_integer()
        {
        }

        constexpr decimal_integer(unsigned long long value)
            : value(value)
        {
        }

        constexpr bool operator==(const decimal_integer& op) const
        {
            return value == op.value;
        }

        constexpr bool operator!=(const decimal_integer& op) const
        {
            return value != op.value;
        }

        constexpr bool operator<(const decimal_integer& op) const
        {
            return value < op.value;
        }

        constexpr bool operator<=(const decimal_integer& op) const
        {
            return value <= op.value;
        }

        constexpr bool operator>=(const decimal_integer& op) const
        {
            return value >= op.value;
        }

        constexpr bool operator>(const decimal_integer& op) const
        {
            return value > op.value;
        }

    private:
        bool parse_chars(const std::string_view& str)
        {
            std::from_chars_result result = std::from_chars(str.data(), str.data() + str.size(), value);
            return result.ec == std::errc{} && result.ptr == str.data() + str.size();
        }

#if (defined(__AVX2__) || defined(__i386__) || defined(__x86_64__))
    private:
        template<std::size_t Size>
        bool parse_integer(const std::string_view& str)
        {
            if (str.size() > Size) {
                if (!parse_simd(str.substr(0, Size))) {
                    return false;
                }
                std::size_t len = str.size() - Size;
                unsigned long long val = value;
                for (std::size_t k = 0; k < len; ++k) {
                    val *= 10;
                }
                if (!parse_chars(str.substr(Size, len))) {
                    return false;
                }
                value += val;
                return true;
            } else {
                return parse_simd(str);
            }
        }
#endif

#if defined(__AVX2__)
    private:
        /** Parses the string representation of an integer with SIMD instructions. */
        bool parse_simd(const std::string_view& str)
        {
            alignas(__m128i) std::array<char, 16> buf = {
                '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
            };
            std::memcpy(buf.data() + 16 - str.size(), str.data(), str.size());
            const __m128i characters = _mm_load_si128(reinterpret_cast<const __m128i*>(buf.data()));

            const __m128i lower_bound = _mm_set1_epi8('0');
            const __m128i upper_bound = _mm_set1_epi8('9');

            const __m128i too_low = _mm_cmpgt_epi8(lower_bound, characters);
            const __m128i too_high = _mm_cmpgt_epi8(characters, upper_bound);
            const __m128i out_of_bounds = _mm_or_si128(too_low, too_high);
            if (_mm_movemask_epi8(out_of_bounds)) {
                return false;
            }

            // convert ASCII characters into digit value (offset from character `0`)
            // '1'  '2'  '3'  '4'  '5'  '6'  '7'  '8'  -->  1  2  3  4  5  6  7  8
            const __m128i values_digit_1 = _mm_sub_epi8(characters, lower_bound);

            // combine pairs of digits into a weighted sum of eight 16-bit integers
            // 1  2  3  4  5  6  7  8  -->  12  34  56  78
            const __m128i scales_10 = _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1);
            const __m128i values_digit_2 = _mm_maddubs_epi16(values_digit_1, scales_10);

            // combine consecutive 16-bit integers into a weighted sum of four 32-bit integers
            // 12  34  56  78  -->  1234  5678
            const __m128i scales_100 = _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1);
            const __m128i values_digit_4 = _mm_madd_epi16(values_digit_2, scales_100);

            // there are no instructions to multiply and horizontally add 32-bit integers
            // pack 32-bit integers into 16-bit slots (no data loss, saturation does not apply)
            // 1234  5678  |  9012  3456  -->  1234  5678  9012  3456  |  0  0  0  0
            const __m128i accumulator = _mm_packs_epi32(values_digit_4, _mm_setzero_si128());

            // combine four 16-bit integers into a weighted sum of two 32-bit integers
            // 1234  5678  9012  3456  -->  12345678  90123456
            const __m128i scales_10000 = _mm_setr_epi16(10000, 1, 10000, 1, 0, 0, 0, 0);
            const __m128i values_digit_8 = _mm_madd_epi16(accumulator, scales_10000);

            // extract 32-bit integer value corresponding to digit quadruplets
            alignas(__m128i) std::array<std::uint32_t, 4> result;
            _mm_store_si128(reinterpret_cast<__m128i*>(result.data()), values_digit_8);
            value = 100'000'000ull * result[0] + result[1];
            return true;
        }

    public:
        bool parse(const std::string_view& str)
        {
            return parse_integer<16>(str);
        }

#elif (defined(__i386__) || defined(__x86_64__))
    private:
        /** Parses the string representation of an integer with SIMD instructions. */
        bool parse_simd(const std::string_view& str)
        {
            char buf[8] = { '0', '0', '0', '0', '0', '0', '0', '0' };
            std::memcpy(buf + 8 - str.size(), str.data(), str.size());
            const __m64 characters = _mm_cvtsi64_m64(*reinterpret_cast<std::int64_t*>(buf));

            const __m64 lower = _mm_set1_pi8('0');
            const __m64 upper = _mm_set1_pi8('9');

            const __m64 too_low = _mm_cmpgt_pi8(lower, characters);
            const __m64 too_high = _mm_cmpgt_pi8(characters, upper);
            if (_mm_movemask_pi8(too_low) | _mm_movemask_pi8(too_high)) {
                return false;
            }

            // get the integer value of each digit
            //   1   2   3   4   5   6   7   8
            const __m64 values_digit1 = _mm_sub_pi8(characters, lower);

            // multiply
            //   1   2   3   4   5   6   7   8
            //  10   1  10   1  10   1  10   1
            // to get
            //  10   2  30   4  50   6  70   8
            //
            // ...and add horizontally:
            //      12      34      56      78
            const __m64 scales_ten = _mm_setr_pi8(10, 1, 10, 1, 10, 1, 10, 1);
            const __m64 values_digit2 = _mm_maddubs_pi16(values_digit1, scales_ten);

            // multiply
            //      12      34      56      78
            //     100       1     100       1
            // to get
            //    1200      34    5600      78
            //
            // ...and add horizontally:
            //            1234            5678
            const __m64 scales_hundred = _mm_setr_pi16(100, 1, 100, 1);
            const __m64 values_digit4 = _mm_madd_pi16(values_digit2, scales_hundred);

            // extract value
            std::int64_t intermediate = _mm_cvtm64_si64(values_digit4);
            value = ((intermediate >> 32) & 0xffffffff) + (intermediate & 0xffffffff) * 10'000;
            return true;
        }

    public:
        bool parse(const std::string_view& str)
        {
            return parse_integer<8>(str);
        }

#else
    public:
        bool parse(const std::string_view& str)
        {
            return parse_chars(str);
        }

#endif

        /** Parses the string representation of a decimal integer into an integer value. */
        bool parse(const char* beg, const char* end)
        {
            return parse(std::string_view(beg, end - beg));
        }

        /** Parses the string representation of a decimal integer into an integer value. */
        bool parse(const char* beg, std::size_t siz)
        {
            return parse(std::string_view(beg, siz));
        }

        unsigned long long value = 0;
    };
}
