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
#include "format.hpp"
#include <string_view>
#include <string>
#include <stdexcept>
#include <cstdio>

namespace simdparse
{
    struct parse_error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    template<typename T>
    T parse(const std::string_view& str)
    {
        T obj;
        if (obj.parse(str)) {
            return obj;
        } else {
            std::array<char, 256> buf;
            int n = std::snprintf(buf.data(), buf.size(), "expected: %s; got: %.32s (len = %zu)", T::name.data(), str.data(), str.size());
            throw parse_error(std::string(buf.data(), buf.data() + n));
        }
    }

    template<typename T>
    T parse(const std::string& str)
    {
        return parse<T>(std::string_view(str.data(), str.size()));
    }

    template<typename T>
    bool parse(T& obj, const std::string_view& str)
    {
        return obj.parse(str);
    }

    template<typename T>
    bool parse(T& obj, const std::string& str)
    {
        return obj.parse(std::string_view(str.data(), str.size()));
    }

    template<typename T>
    void check_parse(const std::string_view& str, const T& ref)
    {
        T obj = parse<T>(str);
        if (obj != ref)
        {
            std::array<char, 256> buf;
            std::string rep = to_string(obj);
            int n = std::snprintf(buf.data(), buf.size(), "expected: %s; got: %.32s (len = %zu)", to_string(ref).c_str(), rep.data(), rep.size());
            throw parse_error(std::string(buf.data(), buf.data() + n));
        }
    }

    template<typename T>
    void check_fail(const std::string_view& str)
    {
        T obj;
        if (obj.parse(str)) {
            std::array<char, 256> buf;
            int n = std::snprintf(buf.data(), buf.size(), "unexpected: parsed %s from: %.32s (len = %zu)", T::name.data(), str.data(), str.size());
            throw parse_error(std::string(buf.data(), buf.data() + n));
        }
    }
}
