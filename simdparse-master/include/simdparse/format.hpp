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
#include "datetime.hpp"
#include "decimal.hpp"
#include "hexadecimal.hpp"
#include "ipaddr.hpp"
#include "uuid.hpp"
#include <string>
#include <cstdlib>

namespace simdparse
{
    inline std::string to_string(const decimal_integer& i)
    {
        return std::to_string(i.value);
    }

    inline std::string to_string(const hexadecimal_integer& i)
    {
        return std::to_string(i.value);
    }

    inline std::string to_string(const ipv4_addr& addr)
    {
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, addr.data(), addr_str, sizeof(addr_str));
        return addr_str;
    }

    inline std::string to_string(const ipv6_addr& addr)
    {
        char addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, addr.data(), addr_str, sizeof(addr_str));
        return addr_str;
    }

    inline std::string to_string(const date& d)
    {
        char buf[16];
        int n = std::snprintf(buf, sizeof(buf), "%.4d-%02u-%02u",
            d.year,
            d.month,
            d.day
        );
        return std::string(buf, buf + n);
    }

    inline std::string to_string(const datetime& dt)
    {
        // 1984-01-01 01:02:03.123456789Z
        char buf[64];
        int n = std::snprintf(buf, sizeof(buf), "%.4d-%02u-%02u %02u:%02u:%02u.%09luZ",
            dt.year,
            dt.month,
            dt.day,
            dt.hour,
            dt.minute,
            dt.second,
            dt.nanosecond
        );
        return std::string(buf, buf + n);
    }

    inline std::string to_string(const microtime& ts)
    {
        std::time_t tv = ts.as_time();
        std::tm* tp = gmtime(&tv);
        if (tp == nullptr) {
            return std::string();
        }

        // 1984-01-01 01:02:03.123456Z
        char buf[32];
        int n = std::snprintf(buf, sizeof(buf), "%.4d-%02u-%02u %02u:%02u:%02u.%06luZ",
            tp->tm_year + 1900,
            tp->tm_mon + 1,
            tp->tm_mday,
            tp->tm_hour,
            tp->tm_min,
            tp->tm_sec,
            ts.microseconds()
        );
        return std::string(buf, buf + n);
    }

    inline std::string to_string(const uuid& u)
    {
        const std::uint8_t* id = u.data();
        char buf[37];
        int n = std::snprintf(buf, sizeof(buf),
            "%2hhx%2hhx%2hhx%2hhx-"
            "%2hhx%2hhx-"
            "%2hhx%2hhx-"
            "%2hhx%2hhx-"
            "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
            id[0], id[1], id[2], id[3],
            id[4], id[5],
            id[6], id[7],
            id[8], id[9],
            id[10], id[11], id[12], id[13], id[14], id[15]
        );
        return std::string(buf, buf + n);
    }
}
