/**
 * simdparse: High-speed parser with vector instructions
 * @see https://github.com/hunyadi/simdparse
 *
 * Copyright (c) 2024 Levente Hunyadi
 *
 * This work is licensed under the terms of the MIT license.
 * For a copy, see <https://opensource.org/licenses/MIT>.
 */

#include <simdparse/base64.hpp>
#include <simdparse/datetime.hpp>
#include <simdparse/decimal.hpp>
#include <simdparse/hexadecimal.hpp>
#include <simdparse/ipaddr.hpp>
#include <simdparse/uuid.hpp>
#include <simdparse/parse.hpp>

template<std::size_t N>
std::string_view to_string_view(const std::array<char, N>& a)
{
    return std::string_view(a.data(), a.size());
}

static bool example1()
{
    using namespace simdparse;
    std::string str = "1984-10-24 23:59:59.123";
    datetime obj;
    if (parse(obj, str)) {
        return true;
    } else {
        return false;
    }
}

static bool example2()
{
    using namespace simdparse;
    std::string str = "1984-10-24 23:59:59.123";
    try {
        auto obj = parse<datetime>(str);
        (void)obj;
    } catch (parse_error&) {
        return false;
    }
    return true;
}

struct assertion_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

inline void check_equals(int val, int ref)
{
    if (val != ref)
    {
        std::array<char, 256> buf;
        int n = std::snprintf(buf.data(), buf.size(), "expected: %d; got: %d", ref, val);
        throw assertion_error(std::string(buf.data(), buf.data() + n));
    }
}

inline void check_equals(unsigned int val, unsigned int ref)
{
    if (val != ref)
    {
        std::array<char, 256> buf;
        int n = std::snprintf(buf.data(), buf.size(), "expected: %u; got: %u", ref, val);
        throw assertion_error(std::string(buf.data(), buf.data() + n));
    }
}

int main(int /*argc*/, char* /*argv*/[])
{
    using simdparse::check_base64url;
    check_base64url("", "");
    check_base64url("f", "Zg");
    check_base64url("fo", "Zm8");
    check_base64url("foo", "Zm9v");
    check_base64url("foob", "Zm9vYg");
    check_base64url("fooba", "Zm9vYmE");
    check_base64url("foobar", "Zm9vYmFy");
    check_base64url(
        std::string_view(
            "\x00\x10\x83\x10\x51\x87\x20\x92\x8b\x30\xd3\x8f\x41\x14\x93\x51"
            "\x55\x97\x61\x96\x9b\x71\xd7\x9f\x82\x18\xa3\x92\x59\xa7\xa2\x9a"
            "\xab\xb2\xdb\xaf\xc3\x1c\xb3\xd3\x5d\xb7\xe3\x9e\xbb\xf3\xdf\xbf",
            48
        ),
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    );
    check_base64url(
        std::string_view(
            "foobar"
            "\x00\x10\x83\x10\x51\x87\x20\x92\x8b\x30\xd3\x8f\x41\x14\x93\x51"
            "\x55\x97\x61\x96\x9b\x71\xd7\x9f\x82\x18\xa3\x92\x59\xa7\xa2\x9a"
            "\xab\xb2\xdb\xaf\xc3\x1c\xb3\xd3\x5d\xb7\xe3\x9e\xbb\xf3\xdf\xbf",
            54
        ),
        "Zm9vYmFyABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    );

    using simdparse::check_parse;
    using simdparse::check_fail;

    using simdparse::date;
    constexpr date d1(1984, 1, 1);
    constexpr date d2(1982, 9, 23);
    static_assert(d1 == d1 && d1 != d2 && !(d1 == d2));
    static_assert(d1 > d2 && d2 < d1);
    static_assert(d2 <= d1 && d1 <= d1 && d1 >= d1 && d1 >= d2);
    check_parse("1984-01-01", d1);
    check_parse("2024-10-24", date(2024, 10, 24));
    check_parse("1000-01-01", date(1000, 1, 1));
    check_fail<date>("YYYY-10-24");
    check_fail<date>("1984-MM-24");
    check_fail<date>("1984-10-DD");
    check_fail<date>("1986-01-99");
    check_fail<date>("1986-99-01");

    using simdparse::tzoffset;
    constexpr tzoffset tz_east(tzoffset::east, 1, 0);
    static_assert(tz_east.minutes() == 60);
    constexpr tzoffset tz_west(tzoffset::west, 1, 30);
    static_assert(tz_west.minutes() == -90);
    static_assert(tz_east == tz_east && tz_east != tz_west && !(tz_east == tz_west));
    static_assert(tz_west < tz_east && tz_east > tz_west);
    static_assert(tz_west <= tz_east && tz_east <= tz_east && tz_east >= tz_east && tz_east >= tz_west);

    using simdparse::datetime;
    constexpr datetime dt(1984, 10, 24, 23, 59, 59, tz_east);
    check_parse("1984-10-24 23:59:59+01:00", dt);
    check_parse("1984-10-24T23:59:59+01:00", dt);

    constexpr datetime dt1(1984, 1, 1, 0, 0, 0);
    constexpr datetime dt2(1982, 10, 24, 23, 59, 59, tzoffset(tzoffset::east, 1, 0));
    static_assert(dt1 == dt1 && dt1 != dt2 && !(dt1 == dt2));
    static_assert(dt1 > dt2 && dt2 < dt1);
    static_assert(dt2 <= dt1 && dt1 <= dt1 && dt1 >= dt1 && dt1 >= dt2);

    // standard fractional part lengths
    check_parse("1984-01-01 01:02:03.000456789+00:00", datetime(1984, 1, 1, 1, 2, 3, 456789));
    check_parse("1984-10-24 23:59:59.123456789+00:00", datetime(1984, 10, 24, 23, 59, 59, 123456789));
    check_parse("1984-01-01 01:02:03.000456+00:00", datetime(1984, 1, 1, 1, 2, 3, 456000));
    check_parse("1984-10-24 23:59:59.123456+00:00", datetime(1984, 10, 24, 23, 59, 59, 123456000));
    check_parse("1984-01-01 01:02:03.456+00:00", datetime(1984, 1, 1, 1, 2, 3, 456000000));
    check_parse("1984-10-24 01:02:03+00:00", datetime(1984, 10, 24, 1, 2, 3));

    // nonstandard fractional part lengths
    check_parse("1984-01-01 01:02:03.0004567+00:00", datetime(1984, 1, 1, 1, 2, 3, 456700));
    check_parse("1984-10-24 23:59:59.1234567+00:00", datetime(1984, 10, 24, 23, 59, 59, 123456700));
    check_parse("1984-01-01 01:02:03.0004+00:00", datetime(1984, 1, 1, 1, 2, 3, 400000));
    check_parse("1984-10-24 23:59:59.1234+00:00", datetime(1984, 10, 24, 23, 59, 59, 123400000));
    check_parse("1984-01-01 01:02:03.4+00:00", datetime(1984, 1, 1, 1, 2, 3, 400000000));

    // time zone offset
    check_parse("1984-01-01 23:59:59.123-11:30", datetime(1984, 1, 1, 23, 59, 59, 123000000, tzoffset(tzoffset::west, 11, 30)));
    check_parse("1984-10-24 01:15:00+02:30", datetime(1984, 10, 24, 1, 15, 0, 0, tzoffset(tzoffset::east, 2, 30)));

    // time zone "Zulu"
    check_parse("1984-01-01 01:02:03.000456789Z", datetime(1984, 1, 1, 1, 2, 3, 456789));
    check_parse("1984-10-24 23:59:59.123456789Z", datetime(1984, 10, 24, 23, 59, 59, 123456789));
    check_parse("1984-01-01 01:02:03.000456Z", datetime(1984, 1, 1, 1, 2, 3, 456000));
    check_parse("1984-01-01 01:02:03.456Z", datetime(1984, 1, 1, 1, 2, 3, 456000000));
    check_parse("1984-01-01 01:02:03Z", datetime(1984, 1, 1, 1, 2, 3, 0));

    // naive date-time string (no time zone designator)
    check_parse("1984-01-01 01:02:03.000456789", datetime(1984, 1, 1, 1, 2, 3, 456789));
    check_parse("1984-01-01 01:02:03.000456", datetime(1984, 1, 1, 1, 2, 3, 456000));
    check_parse("1984-01-01 01:02:03.456", datetime(1984, 1, 1, 1, 2, 3, 456000000));
    check_parse("1984-01-01 01:02:03", datetime(1984, 1, 1, 1, 2, 3, 0));

    // time zone designator "UTC"
    check_parse("1984-10-24 23:59:59.123456 UTC", datetime(1984, 10, 24, 23, 59, 59, 123456000));
    check_parse("1984-10-24 23:59:59.123 UTC", datetime(1984, 10, 24, 23, 59, 59, 123000000));
    check_parse("1984-01-01 01:02:03 UTC", datetime(1984, 1, 1, 1, 2, 3, 0));

    // extreme year values
    check_parse("0001-01-01 00:00:00", datetime(1, 1, 1, 0, 0, 0, 0));
    check_parse("9999-12-31 23:59:59.999999999Z", (datetime::max)());

    // non-numeric characters in date-time strings
    check_fail<datetime>("YYYY-10-24 23:59:59Z");
    check_fail<datetime>("1984-MM-24 23:59:59Z");
    check_fail<datetime>("1984-10-DD 23:59:59Z");
    check_fail<datetime>("1984-10-24 hh:59:59Z");
    check_fail<datetime>("1984-10-24 23:mm:59Z");
    check_fail<datetime>("1984-10-24 23:59:ssZ");
    check_fail<datetime>("1984-10-24 23:59:59+hh:00");
    check_fail<datetime>("1984-10-24 23:59:59+00:mm");
    check_fail<datetime>("1984-10-24 23:59:59.ffffffZ");

    // invalid values for year, month, day, hour, minute or second
    check_fail<datetime>("1984-99-24 23:59:59Z");
    check_fail<datetime>("1984-10-99 23:59:59Z");
    check_fail<datetime>("1984-10-24 30:59:59Z");
    check_fail<datetime>("1984-10-24 23:60:59Z");
    check_fail<datetime>("1984-10-24 23:59:60Z");
    check_fail<datetime>("1984-10-24 23:59:59-01:60");
    check_fail<datetime>("1984-10-24 23:59:59+01:99");

    // wrong separators
    check_fail<datetime>("1984_10_24 23:59:59Z");
    check_fail<datetime>("1984-10-24 23_59_59Z");
    check_fail<datetime>("1984-10-24 23:59:59_01:00");

    // oversized string
    check_fail<datetime>(",2023-03-30T00:36:16.556900+00:00,");

    using simdparse::microtime;
    constexpr microtime mt1 = microtime(10'001'000);  // 10s 1000us
    constexpr microtime mt2 = microtime(20'002'000);  // 20s 2000us
    constexpr microtime mt3 = microtime(-10'001'000);  // -10s 1000us
    static_assert(mt1.value() == 10'001'000);
    static_assert(mt1.microseconds() == 1000);
    static_assert(mt1 == mt1 && mt1 != mt2 && !(mt1 == mt2));
    static_assert(mt2 > mt1 && mt1 < mt2);
    static_assert(mt2 >= mt1 && mt1 >= mt1 && mt1 <= mt1 && mt1 <= mt2);
    static_assert(mt3.microseconds() == 1000);

    // nanosecond truncation
    check_parse("1984-01-01 01:02:03.000456789Z", microtime(1984, 1, 1, 1, 2, 3, 456));
    check_parse("1984-10-24 23:59:59.123456789Z", microtime(1984, 10, 24, 23, 59, 59, 123456));

    // preserve microsecond precision
    check_parse("1984-01-01 01:02:03.000456Z", microtime(1984, 1, 1, 1, 2, 3, 456));
    check_parse("1984-10-24 23:59:59.123456Z", microtime(1984, 10, 24, 23, 59, 59, 123456));

    // add extra precision
    check_parse("1984-01-01 01:02:03.123Z", microtime(1984, 1, 1, 1, 2, 3, 123000));

    // time zone adjustments
    check_parse("1984-10-24 23:59:59.123", microtime(1984, 10, 24, 23, 59, 59, 123000));
    check_parse("1984-01-01 13:02:04.567-11:30", microtime(1984, 1, 2, 0, 32, 4, 567000));
    check_parse("1984-01-01 01:15:00.000+02:30", microtime(1983, 12, 31, 22, 45, 0, 0));

    // extreme year values
    if (microtime(1000, 1, 1, 23, 59, 59).undefined()) {
        throw std::runtime_error("microtime doesn't construct for historical dates");
    }
    if (microtime(9999, 12, 31, 23, 59, 59).undefined()) {
        throw std::runtime_error("microtime doesn't construct for future dates");
    }
    check_parse("1000-01-01 23:59:59", microtime(1000, 1, 1, 23, 59, 59));
    check_parse("1000-01-01 23:59:59.000999", microtime(1000, 1, 1, 23, 59, 59, 999));
    check_parse("1899-01-01 23:59:59", microtime(1899, 1, 1, 23, 59, 59));
    check_parse("1899-01-01 23:59:59.000999", microtime(1899, 1, 1, 23, 59, 59, 999));
    check_parse("9999-12-31 23:59:59", microtime(9999, 12, 31, 23, 59, 59));
    check_parse("9999-12-31 23:59:59.000999", microtime(9999, 12, 31, 23, 59, 59, 999));

    // conversion to date
    check_equals(microtime(1984, 10, 24, 23, 59, 59, 123000).as_date().year, 1984);
    check_equals(microtime(1984, 10, 24, 23, 59, 59, 123000).as_date().month, 10u);
    check_equals(microtime(1984, 10, 24, 23, 59, 59, 123000).as_date().day, 24u);

    // conversion to datetime
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().year, 1984);
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().month, 10u);
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().day, 24u);
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().hour, 23u);
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().minute, 59u);
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().second, 40u);
    check_equals(microtime(1984, 10, 24, 23, 59, 40, 123000).as_datetime().nanosecond, 123000000u);

    // extract fractional seconds
    check_equals(microtime(1984, 10, 24, 23, 59, 59, 123000).microseconds(), 123000);

    using simdparse::ipv4_addr;
    constexpr ipv4_addr sample_ipv4(192, 0, 2, 1);
    check_parse("0.0.0.0", ipv4_addr());
    check_parse("192.0.2.1", sample_ipv4);

    using simdparse::ipv6_addr;
    constexpr ipv6_addr sample_ipv6(0x2001, 0xdb8, 0x0, 0x1234, 0x0, 0x567, 0x8, 0x1);
    if (sample_ipv6 != ipv6_addr(0x20010db8, 0x00001234, 0x00000567, 0x00080001)) {
        throw std::runtime_error("IPv6 operands are not equal");
    }
    if (sample_ipv6 != ipv6_addr(0x20010db800001234, 0x0000056700080001)) {
        throw std::runtime_error("IPv6 operands are not equal");
    }
    check_parse("2001:db8:0:1234:0:567:8:1", sample_ipv6);

    using simdparse::uuid;
    constexpr uuid sample_uuid({ 0xf8, 0x1d, 0x4f, 0xae, 0x7d, 0xec, 0x11, 0xd0, 0xa7, 0x65, 0x00, 0xa0, 0xc9, 0x1e, 0x6b, 0xf6 });
    if (sample_uuid != uuid(0xf81d4fae, 0x7dec11d0, 0xa76500a0, 0xc91e6bf6)) {
        throw std::runtime_error("UUID operands are not equal");
    }
    if (sample_uuid != uuid(0xf81d4fae7dec11d0, 0xa76500a0c91e6bf6)) {
        throw std::runtime_error("UUID operands are not equal");
    }
    check_parse("00000000000000000000000000000000", uuid());
    check_parse("f81d4fae7dec11d0a76500a0c91e6bf6", sample_uuid);
    check_parse("F81D4FAE7DEC11D0A76500A0C91E6BF6", sample_uuid);
    check_parse("00000000-0000-0000-0000-000000000000", uuid());
    check_parse("f81d4fae-7dec-11d0-a765-00a0c91e6bf6", sample_uuid);
    check_parse("F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6", sample_uuid);
    check_parse("{f81d4fae-7dec-11d0-a765-00a0c91e6bf6}", sample_uuid);
    constexpr std::array<char, 32> zero_uuid_str = { '0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0' };
    for (std::size_t k = 0; k < zero_uuid_str.size(); ++k) {
        std::array<char, 32> invalid_uuid_str = zero_uuid_str;
        invalid_uuid_str[k] = 'h';  // illegal hexadecimal
        check_fail<uuid>(to_string_view(invalid_uuid_str));
        invalid_uuid_str[k] = '/';  // character before '0'
        check_fail<uuid>(to_string_view(invalid_uuid_str));
        invalid_uuid_str[k] = ':';  // character after '9'
        check_fail<uuid>(to_string_view(invalid_uuid_str));
        invalid_uuid_str[k] = '@';  // character before 'A'
        check_fail<uuid>(to_string_view(invalid_uuid_str));
        invalid_uuid_str[k] = '[';  // character after 'Z'
        check_fail<uuid>(to_string_view(invalid_uuid_str));
        invalid_uuid_str[k] = '`';  // character before 'a'
        check_fail<uuid>(to_string_view(invalid_uuid_str));
        invalid_uuid_str[k] = '{';  // character after 'z'
        check_fail<uuid>(to_string_view(invalid_uuid_str));
    }

    using simdparse::decimal_integer;
    constexpr decimal_integer i1 = decimal_integer(56);
    constexpr decimal_integer i2 = decimal_integer(84);
    static_assert(i1 == i1 && i1 != i2 && !(i1 == i2));
    static_assert(i1 < i2 && i2 > i1);
    static_assert(i1 <= i2 && i2 <= i2 && i2 >= i2 && i2 >= i1);

    check_parse("0", decimal_integer(0));
    check_parse("1", decimal_integer(1));
    check_parse("9", decimal_integer(9));
    check_parse("12", decimal_integer(12));
    check_parse("123", decimal_integer(123));
    check_parse("1234", decimal_integer(1234));
    check_parse("12345", decimal_integer(12345));
    check_parse("123456", decimal_integer(123456));
    check_parse("1234567", decimal_integer(1234567));
    check_parse("12345678", decimal_integer(12345678));
    check_parse("123456789", decimal_integer(123456789));
    check_parse("1234567890", decimal_integer(1234567890));
    check_parse("1234567812345678", decimal_integer(1234567812345678ull));
    check_parse("123456781234567812", decimal_integer(123456781234567812ull));
    check_parse("12345678123456781234", decimal_integer(12345678123456781234ull));
    check_fail<decimal_integer>("-1");
    check_fail<decimal_integer>("0xab");
    check_fail<decimal_integer>("ff");

    using simdparse::hexadecimal_integer;
    constexpr hexadecimal_integer h1 = hexadecimal_integer(56);
    constexpr hexadecimal_integer h2 = hexadecimal_integer(84);
    static_assert(h1 == h1 && h1 != h2 && !(h1 == h2));
    static_assert(h1 < h2 && h2 > h1);
    static_assert(h1 <= h2 && h2 <= h2 && h2 >= h2 && h2 >= h1);

    check_parse("0", hexadecimal_integer(0));
    check_parse("1", hexadecimal_integer(1));
    check_parse("9", hexadecimal_integer(9));
    check_parse("a", hexadecimal_integer(10));
    check_parse("f", hexadecimal_integer(15));
    check_parse("A", hexadecimal_integer(10));
    check_parse("F", hexadecimal_integer(15));
    check_parse("12", hexadecimal_integer(0x12));
    check_parse("123", hexadecimal_integer(0x123));
    check_parse("1234", hexadecimal_integer(0x1234));
    check_parse("12345", hexadecimal_integer(0x12345));
    check_parse("123456", hexadecimal_integer(0x123456));
    check_parse("1234567", hexadecimal_integer(0x1234567));
    check_parse("12345678", hexadecimal_integer(0x12345678));
    check_parse("123456789", hexadecimal_integer(0x123456789));
    check_parse("123456789a", hexadecimal_integer(0x123456789a));
    check_parse("123456789ab", hexadecimal_integer(0x123456789ab));
    check_parse("123456789abc", hexadecimal_integer(0x123456789abc));
    check_parse("123456789abcd", hexadecimal_integer(0x123456789abcd));
    check_parse("123456789abcde", hexadecimal_integer(0x123456789abcde));
    check_parse("123456789abcdef", hexadecimal_integer(0x123456789abcdef));
    check_parse("fedcba9876543210", hexadecimal_integer(0xfedcba9876543210ull));
    check_parse("0xfedcba9876543210", hexadecimal_integer(0xfedcba9876543210ull));
    check_parse("0xFEDCBA9876543210", hexadecimal_integer(0xfedcba9876543210ull));
    check_fail<hexadecimal_integer>("fedcba9876543210a");

    using simdparse::month_to_ordinal;
    static_assert(month_to_ordinal('J', 'a', 'n') == 1);
    static_assert(month_to_ordinal('F', 'e', 'b') == 2);
    static_assert(month_to_ordinal('M', 'a', 'r') == 3);
    static_assert(month_to_ordinal('A', 'p', 'r') == 4);
    static_assert(month_to_ordinal('M', 'a', 'y') == 5);
    static_assert(month_to_ordinal('J', 'u', 'n') == 6);
    static_assert(month_to_ordinal('J', 'u', 'l') == 7);
    static_assert(month_to_ordinal('A', 'u', 'g') == 8);
    static_assert(month_to_ordinal('S', 'e', 'p') == 9);
    static_assert(month_to_ordinal('O', 'c', 't') == 10);
    static_assert(month_to_ordinal('N', 'o', 'v') == 11);
    static_assert(month_to_ordinal('D', 'e', 'c') == 12);

    static_assert(month_to_ordinal('j', 'a', 'n') == 1);
    static_assert(month_to_ordinal('f', 'e', 'b') == 2);
    static_assert(month_to_ordinal('m', 'a', 'r') == 3);
    static_assert(month_to_ordinal('a', 'p', 'r') == 4);
    static_assert(month_to_ordinal('m', 'a', 'y') == 5);
    static_assert(month_to_ordinal('j', 'u', 'n') == 6);
    static_assert(month_to_ordinal('j', 'u', 'l') == 7);
    static_assert(month_to_ordinal('a', 'u', 'g') == 8);
    static_assert(month_to_ordinal('s', 'e', 'p') == 9);
    static_assert(month_to_ordinal('o', 'c', 't') == 10);
    static_assert(month_to_ordinal('n', 'o', 'v') == 11);
    static_assert(month_to_ordinal('d', 'e', 'c') == 12);

    static_assert(month_to_ordinal(' ', ' ', ' ') == 0);
    static_assert(month_to_ordinal('a', 'b', 'c') == 0);
    static_assert(month_to_ordinal('x', 'y', 'z') == 0);

    // test code examples
    if (!example1() || !example2()) {
        return 1;
    }

    return 0;
}
