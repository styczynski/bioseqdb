#pragma once
#include <string_view>
#include <cstdint>

#if defined(__AVX2__)
#include <array>
#include <cstring>
#include <immintrin.h>
#else
#include <charconv>
#endif

namespace simdparse
{
    struct hexadecimal_integer
    {
        constexpr static std::string_view name = "hexadecimal integer";

        constexpr hexadecimal_integer()
        {
        }

        constexpr hexadecimal_integer(std::uint64_t value)
            : value(value)
        {
        }

        constexpr bool operator==(const hexadecimal_integer& op) const
        {
            return value == op.value;
        }

        constexpr bool operator!=(const hexadecimal_integer& op) const
        {
            return value != op.value;
        }

        constexpr bool operator<(const hexadecimal_integer& op) const
        {
            return value < op.value;
        }

        constexpr bool operator<=(const hexadecimal_integer& op) const
        {
            return value <= op.value;
        }

        constexpr bool operator>=(const hexadecimal_integer& op) const
        {
            return value >= op.value;
        }

        constexpr bool operator>(const hexadecimal_integer& op) const
        {
            return value > op.value;
        }

        /** Parses a hexadecimal string into an integer value. */
        bool parse(const char* beg, const char* end)
        {
            return parse(std::string_view(beg, end - beg));
        }

        /** Parses a hexadecimal string into an integer value. */
        bool parse(const char* beg, std::size_t siz)
        {
            return parse(std::string_view(beg, siz));
        }

        /** Parses a hexadecimal string into an integer value. */
        bool parse(const std::string_view& str)
        {
            if (str.size() > 2) {
                if (str[0] == '0' && str[1] == 'x') {
                    return parse_string(str.substr(2));
                }
            }
            return parse_string(str);
        }

    private:
        bool parse_string(const std::string_view& str)
        {
            if (str.size() > 16) {
                return false;
            }
            return parse_hexadecimal(str);
        }

#if defined(__AVX2__)
        /** Parses the string representation of an integer with SIMD instructions. */
        bool parse_hexadecimal(const std::string_view& str)
        {
            alignas(__m128i) std::array<char, 16> buf;
            std::memset(buf.data(), '0', 16 - str.size());
            std::memcpy(buf.data() + 16 - str.size(), str.data(), str.size());
            const __m128i characters = _mm_load_si128(reinterpret_cast<const __m128i*>(buf.data()));

            const __m128i digit_lower = _mm_cmpgt_epi8(_mm_set1_epi8('0'), characters);
            const __m128i digit_upper = _mm_cmpgt_epi8(characters, _mm_set1_epi8('9'));
            const __m128i is_not_digit = _mm_or_si128(digit_lower, digit_upper);

            // transform to lowercase
            // 0b0011____  (digits 0-9)            -> 0b0011____ (digits)
            // 0b0100____  (uppercase letters A-F) -> 0b0110____ (lowercase)
            // 0b0110____  (lowercase letters a-f) -> 0b0110____ (lowercase)
            const __m128i lowercase_characters = _mm_or_si128(characters, _mm_set1_epi8(0b00100000));
            const __m128i alpha_lower = _mm_cmpgt_epi8(_mm_set1_epi8('a'), lowercase_characters);
            const __m128i alpha_upper = _mm_cmpgt_epi8(lowercase_characters, _mm_set1_epi8('f'));
            const __m128i is_not_alpha = _mm_or_si128(alpha_lower, alpha_upper);

            const __m128i is_not_hex = _mm_and_si128(is_not_digit, is_not_alpha);
            if (_mm_movemask_epi8(is_not_hex)) {
                return false;
            }

            // build a mask to apply a different offset to digit and alpha
            const __m128i digits_offset = _mm_set1_epi8(48);
            const __m128i alpha_offset = _mm_set1_epi8(87);

            // translate ASCII bytes to their value
            // i.e. 0x3132333435363738 -> 0x0102030405060708
            // i.e. 0x3030616263646566 -> 0x00000a0b0c0d0e0f
            const __m128i hex_offset = _mm_blendv_epi8(digits_offset, alpha_offset, is_not_digit);
            __m128i a = _mm_sub_epi8(lowercase_characters, hex_offset);

            // group in 32-bit integer slots, and byte-swap to LSB first
            const __m128i unweave = _mm_set_epi32(0x00020406, 0x01030507, 0x080a0c0e, 0x090b0d0f);
            a = _mm_shuffle_epi8(a, unweave);

            // shift 32-bit slots with high digits
            const __m128i shift = _mm_set_epi32(0x00000004, 0x00000000, 0x00000004, 0x00000000);
            a = _mm_sllv_epi32(a, shift);

            // horizontally add consecutive 32-bit integers to re-combine 64-bit number
            a = _mm_hadd_epi32(a, _mm_setzero_si128());

            _mm_storel_epi64(reinterpret_cast<__m128i*>(&value), a);
            return true;
        }
#else
        bool parse_hexadecimal(const std::string_view& str)
        {
            std::from_chars_result result = std::from_chars(str.data(), str.data() + str.size(), value, 16);
            return result.ec == std::errc{} && result.ptr == str.data() + str.size();
        }
#endif

    public:
        std::uint64_t value = 0;
    };
}
