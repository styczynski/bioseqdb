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
#include <string_view>
#include <cstdint>
#include <cstdio>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace simdparse
{
#if defined(__AVX2__)
    namespace detail
    {
        inline bool parse_uuid(__m256i characters, __m128i& value)
        {
            const __m256i digit_lower = _mm256_cmpgt_epi8(_mm256_set1_epi8('0'), characters);
            const __m256i digit_upper = _mm256_cmpgt_epi8(characters, _mm256_set1_epi8('9'));
            const __m256i is_not_digit = _mm256_or_si256(digit_lower, digit_upper);

            // transform to lowercase
            // 0b0011____  (digits 0-9)            -> 0b0011____ (digits)
            // 0b0100____  (uppercase letters A-F) -> 0b0110____ (lowercase)
            // 0b0110____  (lowercase letters a-f) -> 0b0110____ (lowercase)
            const __m256i lowercase_characters = _mm256_or_si256(characters, _mm256_set1_epi8(0b00100000));
            const __m256i alpha_lower = _mm256_cmpgt_epi8(_mm256_set1_epi8('a'), lowercase_characters);
            const __m256i alpha_upper = _mm256_cmpgt_epi8(lowercase_characters, _mm256_set1_epi8('f'));
            const __m256i is_not_alpha = _mm256_or_si256(alpha_lower, alpha_upper);

            const __m256i is_not_hex = _mm256_and_si256(is_not_digit, is_not_alpha);
            if (_mm256_movemask_epi8(is_not_hex)) {
                return false;
            }

            // build a mask to apply a different offset to digit and alpha
            const __m256i digits_offset = _mm256_set1_epi8(48);
            const __m256i alpha_offset = _mm256_set1_epi8(87);

            // translate ASCII bytes to their value
            // i.e. 0x3132333435363738 -> 0x0102030405060708
            // shift hi-digits
            // i.e. 0x0102030405060708 -> 0x1002300450067008
            // horizontal add
            // i.e. 0x1002300450067008 -> 0x12345678
            const __m256i hex_offset = _mm256_blendv_epi8(digits_offset, alpha_offset, is_not_digit);
            __m256i a = _mm256_sub_epi8(lowercase_characters, hex_offset);
            const __m256i unweave = _mm256_set_epi32(0x0f0d0b09, 0x0e0c0a08, 0x07050301, 0x06040200, 0x0f0d0b09, 0x0e0c0a08, 0x07050301, 0x06040200);
            a = _mm256_shuffle_epi8(a, unweave);
            const __m256i shift = _mm256_set_epi32(0x00000000, 0x00000004, 0x00000000, 0x00000004, 0x00000000, 0x00000004, 0x00000000, 0x00000004);
            a = _mm256_sllv_epi32(a, shift);
            a = _mm256_hadd_epi32(a, _mm256_setzero_si256());
            a = _mm256_permute4x64_epi64(a, 0b00001000);

            value = _mm256_castsi256_si128(a);
            return true;
        }
    }
#endif

    struct uuid
    {
        constexpr static std::string_view name = "UUID";

        constexpr uuid()
        {
        }

        constexpr uuid(const std::array<std::uint8_t, 16>& a)
            : _id(a)
        {
        }

        constexpr uuid(std::uint64_t a, std::uint64_t b)
            : _id{
                static_cast<std::uint8_t>((a >> 56) & 0xff), static_cast<std::uint8_t>((a >> 48) & 0xff), static_cast<std::uint8_t>((a >> 40) & 0xff), static_cast<std::uint8_t>((a >> 32) & 0xff),
                static_cast<std::uint8_t>((a >> 24) & 0xff), static_cast<std::uint8_t>((a >> 16) & 0xff), static_cast<std::uint8_t>((a >> 8) & 0xff), static_cast<std::uint8_t>(a & 0xff),
                static_cast<std::uint8_t>((b >> 56) & 0xff), static_cast<std::uint8_t>((b >> 48) & 0xff), static_cast<std::uint8_t>((b >> 40) & 0xff), static_cast<std::uint8_t>((b >> 32) & 0xff),
                static_cast<std::uint8_t>((b >> 24) & 0xff), static_cast<std::uint8_t>((b >> 16) & 0xff), static_cast<std::uint8_t>((b >> 8) & 0xff), static_cast<std::uint8_t>(b & 0xff) }
        {
        }

        constexpr uuid(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
            : _id{
                static_cast<std::uint8_t>((a >> 24) & 0xff), static_cast<std::uint8_t>((a >> 16) & 0xff), static_cast<std::uint8_t>((a >> 8) & 0xff), static_cast<std::uint8_t>(a & 0xff),
                static_cast<std::uint8_t>((b >> 24) & 0xff), static_cast<std::uint8_t>((b >> 16) & 0xff), static_cast<std::uint8_t>((b >> 8) & 0xff), static_cast<std::uint8_t>(b & 0xff),
                static_cast<std::uint8_t>((c >> 24) & 0xff), static_cast<std::uint8_t>((c >> 16) & 0xff), static_cast<std::uint8_t>((c >> 8) & 0xff), static_cast<std::uint8_t>(c & 0xff),
                static_cast<std::uint8_t>((d >> 24) & 0xff), static_cast<std::uint8_t>((d >> 16) & 0xff), static_cast<std::uint8_t>((d >> 8) & 0xff), static_cast<std::uint8_t>(d & 0xff) }
        {
        }

        const std::uint8_t* data() const
        {
            return _id.data();
        }

        constexpr std::size_t size() const
        {
            return _id.size();
        }

        bool operator==(const uuid& op) const
        {
            return _id == op._id;
        }

        bool operator!=(const uuid& op) const
        {
            return _id != op._id;
        }

        bool operator<(const uuid& op) const
        {
            return _id < op._id;
        }

        bool operator<=(const uuid& op) const
        {
            return _id <= op._id;
        }

        bool operator>=(const uuid& op) const
        {
            return _id >= op._id;
        }

        bool operator>(const uuid& op) const
        {
            return _id > op._id;
        }

        bool parse(const char* beg, const char* end)
        {
            return parse(std::string_view(beg, end - beg));
        }

        bool parse(const char* beg, std::size_t siz)
        {
            return parse(std::string_view(beg, siz));
        }

        /**
         * Converts an UUIDv4 string or a hexadecimal string to a 128-bit unsigned int.
         *
         * UUID string is expected in the 8-4-4-4-12 format, e.g. `f81d4fae-7dec-11d0-a765-00a0c91e6bf6`.
         * The hexadecimal string is expected to have a length of 32 characters.
         */
        bool parse(const std::string_view& str)
        {
            if (str.size() == 38) {  // skip opening and closing curly braces
                return parse_uuid_rfc_4122(str.data() + 1);
            } else if (str.size() == 36) {
                return parse_uuid_rfc_4122(str.data());
            } else if (str.size() == 32) {
                return parse_uuid_compact(str.data());
            }
            return false;
        }

#if defined(__AVX2__)
        bool parse_uuid_compact(const char* str)
        {
            const __m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(str));
            __m128i value;
            if (!detail::parse_uuid(characters, value)) {
                return false;
            }
            _mm_storeu_si128(reinterpret_cast<__m128i*>(_id.data()), value);
            return true;
        }

        /**
         * Converts an UUIDv4 string representation to a 128-bit unsigned int.
         *
         * UUID string is expected in the 8-4-4-4-12 format, e.g. `f81d4fae-7dec-11d0-a765-00a0c91e6bf6`.
         * Uses SIMD via Intel AVX2 instruction set.
         *
         * @see https://github.com/crashoz/uuid_v4
         */
        bool parse_uuid_rfc_4122(const char* str)
        {
            // original hexadecimal digit sequence (as in input string):
            // 01234567-89ab-cdef-FEDC-BA9876543210

            // remove dashes and pack hexadecimal ASCII bytes in a 256-bit integer
            // lane 1: 01234567-89ab-cd -> 0123456789abcd__
            // lane 2: ef-FEDC-BA987654 -> FEDCBA987654____
            const __m256i original = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(str));
            const __m256i dash_shuffle = _mm256_set_epi32(0x80808080, 0x0f0e0d0c, 0x0b0a0908, 0x06050403, 0x80800f0e, 0x0c0b0a09, 0x07060504, 0x03020100);
            const __m256i x = _mm256_shuffle_epi8(original, dash_shuffle);

            // insert characters omitted
            // lane 1: 0123456789abcd__ -> 0123456789abcdef
            // lane 2 is unchanged
            const std::int16_t* p16 = reinterpret_cast<const std::int16_t*>(str + 16);
            const __m256i y = _mm256_insert_epi16(x, *p16, 7);
            // lane 1 is unchanged
            // lane 2: FEDCBA987654____ -> FEDCBA9876543210
            const std::int32_t* p32 = reinterpret_cast<const std::int32_t*>(str + 32);
            const __m256i z = _mm256_insert_epi32(y, *p32, 7);

            __m128i value;
            if (!detail::parse_uuid(z, value)) {
                return false;
            }
            _mm_storeu_si128(reinterpret_cast<__m128i*>(_id.data()), value);
            return true;
        }
#else
        /** Converts a hexadecimal string of 32 characters to a 128-bit unsigned int. */
        bool parse_uuid_compact(const char* str)
        {
            int n = 0;
            sscanf(str,
                "%2hhx%2hhx%2hhx%2hhx"
                "%2hhx%2hhx"
                "%2hhx%2hhx"
                "%2hhx%2hhx"
                "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%n",
                &_id[0], &_id[1], &_id[2], &_id[3],
                &_id[4], &_id[5],
                &_id[6], &_id[7],
                &_id[8], &_id[9],
                &_id[10], &_id[11], &_id[12], &_id[13], &_id[14], &_id[15], &n);
            return n == 32;
        }

        /**
         * Converts an UUIDv4 string representation to a 128-bit unsigned int.
         *
         * UUID string is expected in the 8-4-4-4-12 format, e.g. `f81d4fae-7dec-11d0-a765-00a0c91e6bf6`.
         */
        bool parse_uuid_rfc_4122(const char* str)
        {
            int n = 0;
            std::sscanf(str,
                "%2hhx%2hhx%2hhx%2hhx-"
                "%2hhx%2hhx-"
                "%2hhx%2hhx-"
                "%2hhx%2hhx-"
                "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%n",
                &_id[0], &_id[1], &_id[2], &_id[3],
                &_id[4], &_id[5],
                &_id[6], &_id[7],
                &_id[8], &_id[9],
                &_id[10], &_id[11], &_id[12], &_id[13], &_id[14], &_id[15], &n);
            return n == 36;
        }
#endif

    private:
        std::array<std::uint8_t, 16> _id = { 0 };
    };
}
