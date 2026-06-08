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
#include <string>
#include <string_view>
#include <cstddef>
#include <cstdint>

#if defined(__AVX2__)
#include <array>
#include <cstring>
#include <immintrin.h>
#endif

namespace simdparse
{
    struct base64url
    {
        constexpr static std::string_view name = "modified Base64 for URL";

        static bool encode(const std::basic_string_view<std::byte>& input, std::string& output)
        {
            static constexpr char encoding_table[] = {
                'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
                'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
                'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
                'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
                '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_' };

            std::size_t in_len = input.size();
            std::size_t triplets = in_len / 3;
            std::size_t spare = in_len % 3;
            std::size_t out_len = (4 * triplets) + (spare > 0 ? spare + 1 : 0);

            output.clear();
            output.resize(out_len);

            char* p = output.data();
            unsigned a;
            unsigned b;
            unsigned c;

            std::size_t i = 0;
            for (std::size_t k = 0; k < triplets; i += 3, ++k) {
                a = static_cast<unsigned>(input[i]);
                b = static_cast<unsigned>(input[i + 1]);
                c = static_cast<unsigned>(input[i + 2]);
                *p++ = encoding_table[(a >> 2) & 0x3f];
                *p++ = encoding_table[((a & 0x3) << 4) | ((b >> 4) & 0xf)];
                *p++ = encoding_table[((b & 0xf) << 2) | ((c >> 6) & 0x3)];
                *p++ = encoding_table[c & 0x3f];
            }
            switch (spare) {
            case 1:
                a = static_cast<unsigned>(input[i]);
                *p++ = encoding_table[(a >> 2) & 0x3f];
                *p++ = encoding_table[(a & 0x3) << 4];
                break;
            case 2:
                a = static_cast<unsigned>(input[i]);
                b = static_cast<unsigned>(input[i + 1]);
                *p++ = encoding_table[(a >> 2) & 0x3f];
                *p++ = encoding_table[((a & 0x3) << 4) | ((b >> 4) & 0xf)];
                *p++ = encoding_table[((b & 0xf) << 2)];
                break;
            default:  // case 0:
                break;
            }

            return true;
        }

        static bool encode(const std::basic_string<std::byte>& input, std::string& output)
        {
            return encode(std::basic_string_view<std::byte>(input.data(), input.size()), output);
        }

        static std::string encode(const std::basic_string_view<std::byte>& input)
        {
            std::string output;
            encode(input, output);
            return output;
        }

        static std::string encode(const std::basic_string<std::byte>& input)
        {
            return encode(std::basic_string_view<std::byte>(input.data(), input.size()));
        }

        static bool decode(const std::string_view& input, std::basic_string<std::byte>& output)
        {
            static constexpr unsigned char decoding_table[] = {
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 52, 53, 54, 55, 56, 57,
                58, 59, 60, 61, 64, 64, 64, 64, 64, 64, 64,  0,  1,  2,  3,  4,  5,  6,
                 7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                25, 64, 64, 64, 64, 63, 64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
                37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
                64, 64, 64, 64 };

            std::size_t quadruplets = input.size() / 4;
            std::size_t spare = 0;
            if (input.size() % 4 == 3) {
                spare = 2;
            } else if (input.size() % 4 == 2) {
                spare = 1;
            } else if (input.size() % 4 == 1) {
                return false;
            }

            output.resize(3 * quadruplets + spare);
            std::byte* p = output.data();

            std::size_t i = 0;
            std::size_t j = 0;

#if defined(__AVX2__)
            std::size_t xmms = quadruplets / 8;
            for (std::size_t k = 0; k < xmms; i += 32, j += 8, ++k) {
                if (!decode32(input.data() + i, p)) {
                    return false;
                }
                p += 24;
            }
#endif

            for (; j < quadruplets; i += 4, ++j) {
                unsigned int a = decoding_table[static_cast<int>(input[i])];
                unsigned int b = decoding_table[static_cast<int>(input[i + 1])];
                unsigned int c = decoding_table[static_cast<int>(input[i + 2])];
                unsigned int d = decoding_table[static_cast<int>(input[i + 3])];
                if (((a | b | c | d) & 64) != 0) {
                    return false;
                }

                unsigned int triplet = (a << 3 * 6) | (b << 2 * 6) | (c << 6) | d;
                *p++ = static_cast<std::byte>((triplet >> 2 * 8) & 0xff);
                *p++ = static_cast<std::byte>((triplet >> 1 * 8) & 0xff);
                *p++ = static_cast<std::byte>(triplet & 0xff);
            }

            if (input.size() % 4 == 3) {
                unsigned int a = decoding_table[static_cast<int>(input[i])];
                unsigned int b = decoding_table[static_cast<int>(input[i + 1])];
                unsigned int c = decoding_table[static_cast<int>(input[i + 2])];
                if (((a | b | c) & 64) != 0) {
                    return false;
                }

                unsigned int triplet = (a << 2 * 6) | (b << 6) | c;
                *p++ = static_cast<std::byte>((triplet >> 10) & 0xff);
                *p++ = static_cast<std::byte>((triplet >> 2) & 0xff);
            } else if (input.size() % 4 == 2) {
                unsigned int a = decoding_table[static_cast<int>(input[i])];
                unsigned int b = decoding_table[static_cast<int>(input[i + 1])];
                if (((a | b) & 64) != 0) {
                    return false;
                }

                unsigned int triplet = (a << 6) | b;
                *p++ = static_cast<std::byte>((triplet >> 4) & 0xff);
            }

            return true;
        }

        static bool decode(const std::string& input, std::basic_string<std::byte>& output)
        {
            return decode(std::string_view(input.data(), input.size()), output);
        }

#if defined(__AVX2__)
        static bool decode32(const char* input, std::byte* output)
        {
            const __m256i characters = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input));

            // upper 4 bits of each character
            const __m256i groups = _mm256_and_si256(_mm256_srli_epi32(characters, 4), _mm256_set1_epi8(0x0f));

            // maps lower 4 bits of each character to a mask representing character group membership
            const __m256i valid_mask = _mm256_setr_epi8(
                // first 16 bytes
                0b00010101,
                0b00011111,  // digits, uppercase and lowercase letters
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00001111,  // uppercase and lowercase letters only
                0b00001010,
                0b00001010,
                0b00101010,  // character '-' in group index 2
                0b00001010,
                0b00001110,  // character '_' in group index 5

                // second 16 bytes (copy of first)
                0b00010101,
                0b00011111,  // digits, uppercase and lowercase letters
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00011111,
                0b00001111,  // uppercase and lowercase letters only
                0b00001010,
                0b00001010,
                0b00101010,  // character '-' in group index 2
                0b00001010,
                0b00001110   // character '_' in group index 5
            );
            const __m256i membership = _mm256_shuffle_epi8(valid_mask, characters);

            // maps character group identifier value (upper 4 bits) to a character group bit (1 if member, 0 if not)
            const __m256i group_mask = _mm256_setr_epi8(
                // first 16 bytes
                0b10000000u,
                0b01000000u,
                0b00100000u,
                0b00010000u,
                0b00001000u,
                0b00000100u,
                0b00000010u,
                0b00000001u,
                0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu,  // will match anything

                // second 16 bytes (copy of first)
                0b10000000u,
                0b01000000u,
                0b00100000u,
                0b00010000u,
                0b00001000u,
                0b00000100u,
                0b00000010u,
                0b00000001u,
                0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu
            );
            const __m256i one_hot = _mm256_shuffle_epi8(group_mask, groups);

            // check if any character is out of range for its character class
            if (!_mm256_testc_si256(membership, one_hot)) {
                return false;
            }

            // find the appropriate offset for each character
            const __m256i offset_lookup = _mm256_setr_epi8(
                // first 16 bytes
                64, 64,
                62 - '-',  // '-' maps to index 62
                52 - '0',  // '0'..'9' map to index offset 52
                0 - 'A',   // 'A'..'Z' map to index offset 0
                0 - 'A',
                26 - 'a',  // 'a'..'z' map to index offset 26 (and '_' maps to 63)
                26 - 'a',
                0, 0, 0, 0, 0, 0, 0, 0,

                // second 16 bytes
                64, 64,
                62 - '-',  // '-' maps to index 62
                52 - '0',  // '0'..'9' map to index offset 52
                0 - 'A',   // 'A'..'Z' map to index offset 0
                0 - 'A',
                26 - 'a',  // 'a'..'z' map to index offset 26 (and '_' maps to 63)
                26 - 'a',
                0, 0, 0, 0, 0, 0, 0, 0
            );

            const __m256i offset = _mm256_shuffle_epi8(offset_lookup, groups);
            const __m256i is_underscore = _mm256_cmpeq_epi8(characters, _mm256_set1_epi8('_'));
            const __m256i shift = _mm256_blendv_epi8(offset, _mm256_set1_epi8(63 - '_'), is_underscore);
            const __m256i values = _mm256_add_epi8(characters, shift);

            // values:   00aaaaaa | 00bbbbbb | 00cccccc | 00dddddd
            // multiply: 01000000 | 00000001 | 01000000 | 00000001
            // result:   aabbbbbb   0000aaaa | ccdddddd   0000cccc   (little endian)
            const __m256i merge_ab_and_cd = _mm256_maddubs_epi16(values, _mm256_set1_epi32(0x01400140));

            // values:   aabbbbbb   0000aaaa | ccdddddd   0000cccc   (little endian)
            // multiply: 00000000   00010000 | 00000001   00000000   (little endian)
            // result:   ccdddddd   bbbbcccc   aaaaaabb   00000000   (little endian)
            const __m256i merge_abcd = _mm256_madd_epi16(merge_ab_and_cd, _mm256_set1_epi32(0x00011000));

            // reorder bytes into packed bytes
            const __m256i order = _mm256_setr_epi8(
                2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1,
                2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1
            );
            const __m256i packed_bytes = _mm256_shuffle_epi8(merge_abcd, order);

            alignas(__m256i) std::array<std::uint32_t, 8> result;
            _mm256_store_si256(reinterpret_cast<__m256i*>(result.data()), packed_bytes);

            std::memcpy(output, result.data(), 12);
            std::memcpy(output + 12, result.data() + 4, 12);

            return true;
        }
#endif
    };
}
