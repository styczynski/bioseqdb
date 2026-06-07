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
#include "base64url.hpp"
#include <string_view>
#include <string>
#include <array>
#include <stdexcept>
#include <cstddef>
#include <cstring>

namespace simdparse
{
    inline void check_base64url(const std::string_view& str, const std::string_view& ref)
    {
        std::basic_string<std::byte> in;
        in.resize(str.size());
        std::memcpy(in.data(), str.data(), str.size());
        std::string enc;
        if (!base64url::encode(in, enc)) {
            std::array<char, 256> buf;
            int n = std::snprintf(buf.data(), buf.size(), "expected: %.32s (len = %zu); got encode error", ref.data(), ref.size());
            throw std::runtime_error(std::string(buf.data(), n));
        }
        if (enc != ref) {
            std::array<char, 256> buf;
            int n = std::snprintf(buf.data(), buf.size(), "expected: %.32s (len = %zu); got: %.32s (len = %zu)", ref.data(), ref.size(), enc.data(), enc.size());
            throw std::runtime_error(std::string(buf.data(), n));
        }
        std::basic_string<std::byte> dec;
        if (!base64url::decode(ref, dec)) {
            std::array<char, 256> buf;
            int n = std::snprintf(buf.data(), buf.size(), "expected: %.32s (len = %zu); got decode error", ref.data(), ref.size());
            throw std::runtime_error(std::string(buf.data(), n));
        }
        std::string out;
        out.resize(dec.size());
        std::memcpy(out.data(), dec.data(), dec.size());
        if (out != str) {
            std::array<char, 256> buf;
            int n = std::snprintf(buf.data(), buf.size(), "expected: %.32s (len = %zu); got: %.32s (len = %zu)", str.data(), str.size(), out.data(), out.size());
            throw std::runtime_error(std::string(buf.data(), n));
        }
    }
}
