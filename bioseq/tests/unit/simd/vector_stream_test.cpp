/**
 * @file
 * @brief Compare operations on vector streams with  Highway operations on each slot.
 */

#include <hwy/highway.h>
#include <catch2/catch_template_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.simd.utils;
import bioseq.simd.vector_stream;
import bioseq.common.logging;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Check a binary operation on two streams and with one shared vector.
 * @tparam p_descriptor Highway lane descriptor.
 * @tparam p_stream_width Number of vector slots.
 * @tparam p_lifted Stream operatiod type.
 * @tparam p_highway_operation Highway operation type.
 * @param[in] d Lane descriptor.
 * @param[in] lifted Stream operation to check.
 * @param[in] hn_op Highway reference operation.
 * @param[in] name Operation name for diagnostics.
 */
template <class p_descriptor,
          std::size_t p_stream_width,
          typename p_lifted,
          typename p_highway_operation>
auto check_binary_vec_op(p_descriptor d,
                         p_lifted lifted,
                         p_highway_operation hn_op,
                         const char* name) -> void {
    using lane_type = hn::TFromD<p_descriptor>;
    const auto va = hn::Set(d, static_cast<lane_type>(0xAA));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x55));
    const auto vc = hn::Set(d, static_cast<lane_type>(0x12));

    auto sa = vector_stream<p_descriptor, lane_type, p_stream_width>::pack(va, vb);
    auto sb = vector_stream<p_descriptor, lane_type, p_stream_width>::pack(vb, vc);

    {
        auto rs = lifted(sa, sb);
        for (std::size_t k = 0; k < p_stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn_op(sa[k], sb[k]));
            ASSERT(got == want);
        }
    }
    {
        auto rs = lifted(sa, vc);
        for (std::size_t k = 0; k < p_stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn_op(sa[k], vc));
            ASSERT(got == want);
        }
    }
    {
        auto rs = lifted(va, sb);
        for (std::size_t k = 0; k < p_stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn_op(va, sb[k]));
            ASSERT(got == want);
        }
    }
    bioseq_log<logging_level::debug>("[vector_stream binary] op={} OK", name);
}

/**
 * @brief Compare stream masks with Highway masks after conversion to vectors.
 * @tparam p_descriptor Highway lane descriptor.
 * @tparam p_stream_width Number of mask slots.
 * @tparam p_lifted Stream comparison type.
 * @tparam p_highway_operation Highway comparison type.
 * @param[in] d Lane descriptor.
 * @param[in] lifted Stream  comparison to check.
 * @param[in] hn_op Highway reference comparison.
 * @param[in] name Operation name for diagnostics.
 */
template <class p_descriptor,
          std::size_t p_stream_width,
          typename p_lifted,
          typename p_highway_operation>
auto check_binary_cmp_op(p_descriptor d,
                         p_lifted lifted,
                         p_highway_operation hn_op,
                         const char* name) -> void {
    using lane_type = hn::TFromD<p_descriptor>;
    const auto va = hn::Set(d, static_cast<lane_type>(5));
    const auto vb = hn::Set(d, static_cast<lane_type>(10));

    auto sa = vector_stream<p_descriptor, lane_type, p_stream_width>::pack(va, vb);
    auto sb = vector_stream<p_descriptor, lane_type, p_stream_width>::pack(vb, va);

    auto ms = lifted(sa, sb);
    for (std::size_t k = 0; k < p_stream_width; ++k) {
        const auto got = extract_lanes<lane_type>(d, hn::VecFromMask(d, ms[k]));
        const auto want = extract_lanes<lane_type>(d, hn::VecFromMask(d, hn_op(sa[k], sb[k])));
        ASSERT(got == want);
    }
    bioseq_log<logging_level::debug>("[vector_stream cmp] op={} OK", name);
}



TEMPLATE_TEST_CASE("vector_stream binary arithmetic equivalence",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return add(a, b); },
        [](auto&& a, auto&& b) { return hn::Add(a, b); },
        "Add");
    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return subtract(a, b); },
        [](auto&& a, auto&& b) { return hn::Sub(a, b); },
        "Sub");
    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return minimum(a, b); },
        [](auto&& a, auto&& b) { return hn::Min(a, b); },
        "Min");
    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return maximum(a, b); },
        [](auto&& a, auto&& b) { return hn::Max(a, b); },
        "Max");
}

TEMPLATE_TEST_CASE("vector_stream binary bitwise equivalence",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return bit_and(a, b); },
        [](auto&& a, auto&& b) { return hn::And(a, b); },
        "And");
    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return bit_or(a, b); },
        [](auto&& a, auto&& b) { return hn::Or(a, b); },
        "Or");
    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return bit_xor(a, b); },
        [](auto&& a, auto&& b) { return hn::Xor(a, b); },
        "Xor");
    check_binary_vec_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return bit_and_not(a, b); },
        [](auto&& a, auto&& b) { return hn::AndNot(a, b); },
        "AndNot");
}

TEMPLATE_TEST_CASE("mask_stream AndNot broadcasts a plain mask",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    // The two-stack collector applies one live-lane mask to every stream
    // slot. Check that broadcasting preserves Highway's AndNot result.
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    const auto pal_a = hn::Eq(hn::Set(d, static_cast<lane_type>(5)),
                              hn::Set(d, static_cast<lane_type>(5))); // all-true
    const auto pal_b = hn::Ne(hn::Set(d, static_cast<lane_type>(5)),
                              hn::Set(d, static_cast<lane_type>(5))); // all-false
    const auto live = hn::Eq(hn::Set(d, static_cast<lane_type>(7)),
                             hn::Set(d, static_cast<lane_type>(7))); // all-true

    auto pal_stream = mask_stream<decltype(d), 2>::pack(pal_a, pal_b);

    // Apply AndNot with the same plain mask in every slot.
    auto result_stream = bit_and_not(pal_stream, live);
    for (std::size_t s = 0; s < 2; ++s) {
        const auto got = extract_lanes<lane_type>(d, hn::VecFromMask(d, result_stream[s]));
        const auto want =
            extract_lanes<lane_type>(d, hn::VecFromMask(d, hn::AndNot(pal_stream[s], live)));
        ASSERT(got == want);
    }
    bioseq_log<logging_level::debug>("[mask_stream AndNot+broadcast] OK");
}

TEMPLATE_TEST_CASE("vector_stream binary comparisons",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    check_binary_cmp_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return equal(a, b); },
        [](auto&& a, auto&& b) { return hn::Eq(a, b); },
        "Eq");
    check_binary_cmp_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return not_equal(a, b); },
        [](auto&& a, auto&& b) { return hn::Ne(a, b); },
        "Ne");
    check_binary_cmp_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return less(a, b); },
        [](auto&& a, auto&& b) { return hn::Lt(a, b); },
        "Lt");
    check_binary_cmp_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return greater(a, b); },
        [](auto&& a, auto&& b) { return hn::Gt(a, b); },
        "Gt");
    check_binary_cmp_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return less_equal(a, b); },
        [](auto&& a, auto&& b) { return hn::Le(a, b); },
        "Le");
    check_binary_cmp_op<decltype(d), 2>(
        d,
        [](auto&& a, auto&& b) { return greater_equal(a, b); },
        [](auto&& a, auto&& b) { return hn::Ge(a, b); },
        "Ge");
}

TEMPLATE_TEST_CASE("vector_stream templated shifts",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d, static_cast<lane_type>(0x35));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x91));
    auto sa = vector_stream<decltype(d), lane_type, stream_width>::pack(va, vb);

    {
        auto rs = shift_left<3>(sa);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::ShiftLeft<3>(sa[k]));
            ASSERT(got == want);
        }
    }
    {
        auto rs = shift_right<2>(sa);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::ShiftRight<2>(sa[k]));
            ASSERT(got == want);
        }
    }
}

TEMPLATE_TEST_CASE("vector_stream runtime same-amount shifts",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d, static_cast<lane_type>(0x35));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x91));
    auto sa = vector_stream<decltype(d), lane_type, stream_width>::pack(va, vb);
    const int amt = 3;

    {
        auto rs = shift_left_same(sa, amt);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::ShiftLeftSame(sa[k], amt));
            ASSERT(got == want);
        }
    }
    {
        auto rs = shift_right_same(sa, amt);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::ShiftRightSame(sa[k], amt));
            ASSERT(got == want);
        }
    }
}

TEMPLATE_TEST_CASE("vector_stream if_then_else — every argument shape",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto m_a = hn::Lt(hn::Set(d, lane_type{1}), hn::Set(d, lane_type{2})); // all-true
    const auto m_b = hn::Lt(hn::Set(d, lane_type{2}), hn::Set(d, lane_type{1})); // all-false
    auto ms = mask_stream<decltype(d), stream_width>::pack(m_a, m_b);

    const auto va = hn::Set(d, static_cast<lane_type>(0xAA));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x55));
    auto sa = vector_stream<decltype(d), lane_type, stream_width>::pack(va, vb);
    auto sb = vector_stream<decltype(d), lane_type, stream_width>::pack(vb, va);

    // Each slot has its own mask and both vectors.
    {
        auto rs = if_then_else(ms, sa, sb);
        static_assert(k_is_vector_stream<decltype(rs)>);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::IfThenElse(ms[k], sa[k], sb[k]));
            ASSERT(got == want);
        }
    }

    // Share the false-case vector across slots.
    {
        auto rs = if_then_else(ms, sa, vb);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::IfThenElse(ms[k], sa[k], vb));
            ASSERT(got == want);
        }
    }

    // Share the true-case vector across slots.
    {
        auto rs = if_then_else(ms, va, sb);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::IfThenElse(ms[k], va, sb[k]));
            ASSERT(got == want);
        }
    }

    // Share both vectors while using each slot's mask.
    {
        auto rs = if_then_else(ms, va, vb);
        static_assert(k_is_vector_stream<decltype(rs)>,
                      "if_then_else(mask_stream, vec, vec) must produce vector_stream");
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::IfThenElse(ms[k], va, vb));
            ASSERT(got == want);
        }
    }

    // Share, one mask across both vector streams.
    {
        auto rs = if_then_else(m_a, sa, sb);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = extract_lanes<lane_type>(d, rs[k]);
            const auto want = extract_lanes<lane_type>(d, hn::IfThenElse(m_a, sa[k], sb[k]));
            ASSERT(got == want);
        }
    }

    bioseq_log<logging_level::debug>("[vector_stream if_then_else] all 5 shapes OK");
}

TEMPLATE_TEST_CASE("vector_stream static factories",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    auto z = vector_stream<decltype(d), lane_type, stream_width>::splat_zero(d);
    const auto zero_lanes = extract_lanes<lane_type>(d, hn::Zero(d));
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = extract_lanes<lane_type>(d, z[k]);
        ASSERT(got == zero_lanes);
    }

    const lane_type val = static_cast<lane_type>(0x12);
    auto s = vector_stream<decltype(d), lane_type, stream_width>::splat(d, val);
    const auto val_lanes = extract_lanes<lane_type>(d, hn::Set(d, val));
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = extract_lanes<lane_type>(d, s[k]);
        ASSERT(got == val_lanes);
    }
}

TEMPLATE_TEST_CASE("mask_stream static factory splat_false",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    auto m = mask_stream<decltype(d), stream_width>::splat_false(d);
    for (std::size_t k = 0; k < stream_width; ++k) {
        ASSERT(hn::AllFalse(d, m[k]));
    }
}

TEMPLATE_TEST_CASE("vector_stream bit_not / negate / absolute",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d, static_cast<lane_type>(0x35));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x91));
    auto sa = vector_stream<decltype(d), lane_type, stream_width>::pack(va, vb);

    auto rs = bit_not(sa);
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = extract_lanes<lane_type>(d, rs[k]);
        const auto want = extract_lanes<lane_type>(d, hn::Not(sa[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vector_stream bit_cast across types",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint32_t) {
    const hn::ScalableTag<std::uint32_t> d_in;
    const hn::ScalableTag<std::uint8_t> d_out;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d_in, std::uint32_t{0xDEADBEEF});
    const auto vb = hn::Set(d_in, std::uint32_t{0x12345678});
    auto sa = vector_stream<decltype(d_in), std::uint32_t, stream_width>::pack(va, vb);

    auto rs = bit_cast(d_out, sa);
    static_assert(k_is_vector_stream<decltype(rs)>);
    static_assert(
        std::is_same_v<typename decltype(rs)::tag_type, std::remove_cvref_t<decltype(d_out)>>);

    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = extract_lanes<std::uint8_t>(d_out, rs[k]);
        const auto want = extract_lanes<std::uint8_t>(d_out, hn::BitCast(d_out, sa[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vector_stream vector_from_mask round-trip",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto m_a = hn::Lt(hn::Set(d, lane_type{1}), hn::Set(d, lane_type{2})); // all-true
    const auto m_b = hn::Lt(hn::Set(d, lane_type{2}), hn::Set(d, lane_type{1})); // all-false
    auto ms = mask_stream<decltype(d), stream_width>::pack(m_a, m_b);

    auto vs = vector_from_mask(d, ms);
    static_assert(k_is_vector_stream<decltype(vs)>);
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = extract_lanes<lane_type>(d, vs[k]);
        const auto want = extract_lanes<lane_type>(d, hn::VecFromMask(d, ms[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vector_stream mask_from_vector round-trip",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto v_all_ones = hn::Set(d, static_cast<lane_type>(static_cast<lane_type>(-1)));
    const auto v_zero = hn::Zero(d);
    auto sa = vector_stream<decltype(d), lane_type, stream_width>::pack(v_all_ones, v_zero);

    auto ms = mask_from_vector(sa);
    static_assert(k_is_mask_stream<decltype(ms)>);
    // All-one bits produce  a true mask in the frst slot. Zero bits produce
    // a false mask in the second
    ASSERT(!hn::AllFalse(d, ms[0]));
    ASSERT(hn::AllFalse(d, ms[1]));
}

TEMPLATE_TEST_CASE("vector_stream store_stream",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t,
                   std::uint32_t,
                   std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    HWY_ALIGN std::array<lane_type, hn::MaxLanes(d)> a{};
    HWY_ALIGN std::array<lane_type, hn::MaxLanes(d)> b{};

    const lane_type val_a = static_cast<lane_type>(0xAA);
    const lane_type val_b = static_cast<lane_type>(0xBB);
    auto s = vector_stream<decltype(d), lane_type, stream_width>::pack(hn::Set(d, val_a),
                                                                       hn::Set(d, val_b));

    store_stream(s, d, std::array<lane_type*, stream_width>{a.data(), b.data()});

    for (std::size_t i = 0; i < hn::Lanes(d); ++i) {
        ASSERT(a[i] == val_a);
        ASSERT(b[i] == val_b);
    }
}

TEMPLATE_TEST_CASE("vector_stream collect_all_false_per_stream",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint32_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto m_false = hn::MaskFalse(d);
    const auto m_first = hn::FirstN(d, 1); // Only the first lane is true.
    auto ms = mask_stream<decltype(d), stream_width>::pack(m_false, m_first);

    const auto per = collect_all_false_per_stream(d, ms);
    ASSERT(per[0] == true);
    ASSERT(per[1] == false);
}

TEMPLATE_TEST_CASE("vector_stream stream count 1 collapses cleanly",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint32_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 1;

    const auto va = hn::Set(d, static_cast<lane_type>(7));
    const auto vb = hn::Set(d, static_cast<lane_type>(11));
    auto sa = vector_stream<decltype(d), lane_type, stream_width>::pack(va);
    auto sb = vector_stream<decltype(d), lane_type, stream_width>::pack(vb);

    auto rs = add(sa, sb);
    static_assert(decltype(rs)::k_stream_count == 1);
    const auto got = extract_lanes<lane_type>(d, rs[0]);
    const auto want = extract_lanes<lane_type>(d, hn::Add(va, vb));
    ASSERT(got == want);
}

// Check copy and destruction properties at compile time.

static_assert(
    std::is_trivially_copyable_v<vector_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>,
    "vector_stream must be trivially copyable");
static_assert(std::is_trivially_destructible_v<
                  vector_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>,
              "vector_stream must be trivially destructible");
static_assert(std::is_trivially_copyable_v<mask_stream<hn::ScalableTag<std::uint32_t>, 2>>,
              "mask_stream must be trivially copyable");

// Check which types and argument lists contain streams.
static_assert(k_is_vector_stream<vector_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>);
static_assert(!k_is_vector_stream<hn::Vec<hn::ScalableTag<std::uint32_t>>>);
static_assert(k_is_mask_stream<mask_stream<hn::ScalableTag<std::uint32_t>, 2>>);
static_assert(!k_is_mask_stream<hn::Mask<hn::ScalableTag<std::uint32_t>>>);
static_assert(k_is_any_stream<vector_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>);
static_assert(k_is_any_stream<mask_stream<hn::ScalableTag<std::uint32_t>, 2>>);
static_assert(
    has_stream_argument<vector_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>, int>);
static_assert(!has_stream_argument<int, double>);

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
