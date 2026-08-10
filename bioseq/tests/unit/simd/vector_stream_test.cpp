#include <tests/support/common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <hwy/highway.h>

import bioseq.simd.utils;
import bioseq.simd.vector_stream;
import bioseq.common.logging;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// Equivalence-test harness
//
// For each lifted op, we verify three argument shapes against the equivalent
// direct hn::* call:
//   stream(s) ⊕ stream(s)
//   stream(s) ⊕ plain_vec   (broadcast right)
//   plain_vec ⊕ stream(s)   (broadcast left)
// and assert per-slot byte-equality.
// =============================================================================

template <class p_descriptor, std::size_t p_stream_width,
          typename p_lifted, typename p_highway_operation>
void check_binary_vec_op(
        p_descriptor d,
        p_lifted lifted,
        p_highway_operation hn_op,
        const char* name) {
    using lane_type = hn::TFromD<p_descriptor>;
    const auto va = hn::Set(d, static_cast<lane_type>(0xAA));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x55));
    const auto vc = hn::Set(d, static_cast<lane_type>(0x12));

    auto sa = vec_stream<
        p_descriptor, lane_type, p_stream_width>::pack(va, vb);
    auto sb = vec_stream<
        p_descriptor, lane_type, p_stream_width>::pack(vb, vc);

    {
        auto rs = lifted(sa, sb);
        for (std::size_t k = 0; k < p_stream_width; ++k) {
            const auto got = hwy_to_vector<p_descriptor, lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<p_descriptor, lane_type>(
                d, hn_op(sa[k], sb[k]));
            ASSERT(got == want);
        }
    }
    {
        auto rs = lifted(sa, vc);
        for (std::size_t k = 0; k < p_stream_width; ++k) {
            const auto got = hwy_to_vector<p_descriptor, lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<p_descriptor, lane_type>(
                d, hn_op(sa[k], vc));
            ASSERT(got == want);
        }
    }
    {
        auto rs = lifted(va, sb);
        for (std::size_t k = 0; k < p_stream_width; ++k) {
            const auto got = hwy_to_vector<p_descriptor, lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<p_descriptor, lane_type>(
                d, hn_op(va, sb[k]));
            ASSERT(got == want);
        }
    }
    bioseq_log<logging_level::debug>("[vec_stream binary] op={} OK", name);
}

// Comparison ops: result is a mask_stream. Compare via VecFromMask round-trip.
template <class p_descriptor, std::size_t p_stream_width,
          typename p_lifted, typename p_highway_operation>
void check_binary_cmp_op(
        p_descriptor d,
        p_lifted lifted,
        p_highway_operation hn_op,
        const char* name) {
    using lane_type = hn::TFromD<p_descriptor>;
    const auto va = hn::Set(d, static_cast<lane_type>(5));
    const auto vb = hn::Set(d, static_cast<lane_type>(10));

    auto sa = vec_stream<
        p_descriptor, lane_type, p_stream_width>::pack(va, vb);
    auto sb = vec_stream<
        p_descriptor, lane_type, p_stream_width>::pack(vb, va);

    auto ms = lifted(sa, sb);
    for (std::size_t k = 0; k < p_stream_width; ++k) {
        const auto got = hwy_to_vector<p_descriptor, lane_type>(
            d, hn::VecFromMask(d, ms[k]));
        const auto want = hwy_to_vector<p_descriptor, lane_type>(
            d, hn::VecFromMask(d, hn_op(sa[k], sb[k])));
        ASSERT(got == want);
    }
    bioseq_log<logging_level::debug>("[vec_stream cmp] op={} OK", name);
}

// =============================================================================
// Tests — one TEST_CASE per op family for clean failure isolation.
// =============================================================================

TEMPLATE_TEST_CASE("vec_stream binary arithmetic equivalence",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Add(a, b); },
        [](auto&& a, auto&& b){ return hn::Add(a, b); }, "Add");
    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Sub(a, b); },
        [](auto&& a, auto&& b){ return hn::Sub(a, b); }, "Sub");
    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Min(a, b); },
        [](auto&& a, auto&& b){ return hn::Min(a, b); }, "Min");
    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Max(a, b); },
        [](auto&& a, auto&& b){ return hn::Max(a, b); }, "Max");
}

TEMPLATE_TEST_CASE("vec_stream binary bitwise equivalence",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return And(a, b); },
        [](auto&& a, auto&& b){ return hn::And(a, b); }, "And");
    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Or(a, b); },
        [](auto&& a, auto&& b){ return hn::Or(a, b); }, "Or");
    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Xor(a, b); },
        [](auto&& a, auto&& b){ return hn::Xor(a, b); }, "Xor");
    check_binary_vec_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return AndNot(a, b); },
        [](auto&& a, auto&& b){ return hn::AndNot(a, b); }, "AndNot");
}

TEMPLATE_TEST_CASE("mask_stream AndNot broadcasts a plain mask",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    // The two-stack collector combines a mask stream with one live-lane mask.
    // Verify that the plain mask is broadcast to each stream slot.
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    const auto pal_a = hn::Eq(hn::Set(d, static_cast<lane_type>(5)),
                              hn::Set(d, static_cast<lane_type>(5)));   // all-true
    const auto pal_b = hn::Ne(hn::Set(d, static_cast<lane_type>(5)),
                              hn::Set(d, static_cast<lane_type>(5)));   // all-false
    const auto live = hn::Eq(hn::Set(d, static_cast<lane_type>(7)),
                             hn::Set(d, static_cast<lane_type>(7)));    // all-true

    auto pal_stream = mask_stream<decltype(d), 2>::pack(pal_a, pal_b);

    // Lifted: mask_stream ⊕ plain mask
    auto result_stream = AndNot(pal_stream, live);
    for (std::size_t s = 0; s < 2; ++s) {
        const auto got  = hwy_to_vector<decltype(d), lane_type>(d,
            hn::VecFromMask(d, result_stream[s]));
        const auto want = hwy_to_vector<decltype(d), lane_type>(d,
            hn::VecFromMask(d, hn::AndNot(pal_stream[s], live)));
        ASSERT(got == want);
    }
    bioseq_log<logging_level::debug>("[mask_stream AndNot+broadcast] OK");
}

TEMPLATE_TEST_CASE("vec_stream binary comparisons",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;

    check_binary_cmp_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Eq(a, b); },
        [](auto&& a, auto&& b){ return hn::Eq(a, b); }, "Eq");
    check_binary_cmp_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Ne(a, b); },
        [](auto&& a, auto&& b){ return hn::Ne(a, b); }, "Ne");
    check_binary_cmp_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Lt(a, b); },
        [](auto&& a, auto&& b){ return hn::Lt(a, b); }, "Lt");
    check_binary_cmp_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Gt(a, b); },
        [](auto&& a, auto&& b){ return hn::Gt(a, b); }, "Gt");
    check_binary_cmp_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Le(a, b); },
        [](auto&& a, auto&& b){ return hn::Le(a, b); }, "Le");
    check_binary_cmp_op<decltype(d), 2>(d,
        [](auto&& a, auto&& b){ return Ge(a, b); },
        [](auto&& a, auto&& b){ return hn::Ge(a, b); }, "Ge");
}

TEMPLATE_TEST_CASE("vec_stream templated shifts",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d, static_cast<lane_type>(0x35));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x91));
    auto sa = vec_stream<decltype(d), lane_type, stream_width>::pack(va, vb);

    {
        auto rs = ShiftLeft<3>(sa);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::ShiftLeft<3>(sa[k]));
            ASSERT(got == want);
        }
    }
    {
        auto rs = ShiftRight<2>(sa);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::ShiftRight<2>(sa[k]));
            ASSERT(got == want);
        }
    }
}

TEMPLATE_TEST_CASE("vec_stream runtime same-amount shifts",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d, static_cast<lane_type>(0x35));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x91));
    auto sa = vec_stream<decltype(d), lane_type, stream_width>::pack(va, vb);
    const int amt = 3;

    {
        auto rs = ShiftLeftSame(sa, amt);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::ShiftLeftSame(sa[k], amt));
            ASSERT(got == want);
        }
    }
    {
        auto rs = ShiftRightSame(sa, amt);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::ShiftRightSame(sa[k], amt));
            ASSERT(got == want);
        }
    }
}

TEMPLATE_TEST_CASE("vec_stream IfThenElse — every argument shape",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto m_a = hn::Lt(hn::Set(d, lane_type{1}), hn::Set(d, lane_type{2}));   // all-true
    const auto m_b = hn::Lt(hn::Set(d, lane_type{2}), hn::Set(d, lane_type{1}));   // all-false
    auto ms = mask_stream<decltype(d), stream_width>::pack(m_a, m_b);

    const auto va = hn::Set(d, static_cast<lane_type>(0xAA));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x55));
    auto sa = vec_stream<decltype(d), lane_type, stream_width>::pack(va, vb);
    auto sb = vec_stream<decltype(d), lane_type, stream_width>::pack(vb, va);

    // (mask_stream, vec_stream, vec_stream)
    {
        auto rs = IfThenElse(ms, sa, sb);
        static_assert(is_vec_stream_v<decltype(rs)>);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d,
                hn::IfThenElse(ms[k], sa[k], sb[k]));
            ASSERT(got == want);
        }
    }

    // (mask_stream, vec_stream, plain_vec)
    {
        auto rs = IfThenElse(ms, sa, vb);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d,
                hn::IfThenElse(ms[k], sa[k], vb));
            ASSERT(got == want);
        }
    }

    // (mask_stream, plain_vec, vec_stream)
    {
        auto rs = IfThenElse(ms, va, sb);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d,
                hn::IfThenElse(ms[k], va, sb[k]));
            ASSERT(got == want);
        }
    }

    // (mask_stream, plain_vec, plain_vec) — broadcast both vectors.
    {
        auto rs = IfThenElse(ms, va, vb);
        static_assert(is_vec_stream_v<decltype(rs)>,
                      "IfThenElse(mask_stream, vec, vec) must produce vec_stream");
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d,
                hn::IfThenElse(ms[k], va, vb));
            ASSERT(got == want);
        }
    }

    // (plain_mask, vec_stream, vec_stream) — broadcast mask
    {
        auto rs = IfThenElse(m_a, sa, sb);
        for (std::size_t k = 0; k < stream_width; ++k) {
            const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), lane_type>(d,
                hn::IfThenElse(m_a, sa[k], sb[k]));
            ASSERT(got == want);
        }
    }

    bioseq_log<logging_level::debug>("[vec_stream IfThenElse] all 5 shapes OK");
}

TEMPLATE_TEST_CASE("vec_stream static factories",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    auto z = vec_stream<decltype(d), lane_type, stream_width>::splat_zero(d);
    const auto zero_lanes = hwy_to_vector<decltype(d), lane_type>(d, hn::Zero(d));
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = hwy_to_vector<decltype(d), lane_type>(d, z[k]);
        ASSERT(got == zero_lanes);
    }

    const lane_type val = static_cast<lane_type>(0x12);
    auto s = vec_stream<decltype(d), lane_type, stream_width>::splat(d, val);
    const auto val_lanes = hwy_to_vector<decltype(d), lane_type>(d, hn::Set(d, val));
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = hwy_to_vector<decltype(d), lane_type>(d, s[k]);
        ASSERT(got == val_lanes);
    }
}

TEMPLATE_TEST_CASE("mask_stream static factory splat_false",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    auto m = mask_stream<decltype(d), stream_width>::splat_false(d);
    for (std::size_t k = 0; k < stream_width; ++k) {
        ASSERT(hn::AllFalse(d, m[k]));
    }
}

TEMPLATE_TEST_CASE("vec_stream Not / Neg / Abs",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d, static_cast<lane_type>(0x35));
    const auto vb = hn::Set(d, static_cast<lane_type>(0x91));
    auto sa = vec_stream<decltype(d), lane_type, stream_width>::pack(va, vb);

    auto rs = Not(sa);
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[k]);
        const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::Not(sa[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vec_stream BitCast across types",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint32_t) {
    const hn::ScalableTag<std::uint32_t> d_in;
    const hn::ScalableTag<std::uint8_t> d_out;
    constexpr std::size_t stream_width = 2;

    const auto va = hn::Set(d_in, std::uint32_t{0xDEADBEEF});
    const auto vb = hn::Set(d_in, std::uint32_t{0x12345678});
    auto sa = vec_stream<decltype(d_in), std::uint32_t, stream_width>::pack(va, vb);

    auto rs = BitCast(d_out, sa);
    static_assert(is_vec_stream_v<decltype(rs)>);
    static_assert(std::is_same_v<typename decltype(rs)::tag_t,
                                   std::remove_cvref_t<decltype(d_out)>>);

    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = hwy_to_vector<decltype(d_out), std::uint8_t>(d_out, rs[k]);
        const auto want = hwy_to_vector<decltype(d_out), std::uint8_t>(d_out,
            hn::BitCast(d_out, sa[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vec_stream VecFromMask round-trip",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto m_a = hn::Lt(hn::Set(d, lane_type{1}), hn::Set(d, lane_type{2}));   // all-true
    const auto m_b = hn::Lt(hn::Set(d, lane_type{2}), hn::Set(d, lane_type{1}));   // all-false
    auto ms = mask_stream<decltype(d), stream_width>::pack(m_a, m_b);

    auto vs = VecFromMask(d, ms);
    static_assert(is_vec_stream_v<decltype(vs)>);
    for (std::size_t k = 0; k < stream_width; ++k) {
        const auto got = hwy_to_vector<decltype(d), lane_type>(d, vs[k]);
        const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::VecFromMask(d, ms[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vec_stream MaskFromVec round-trip",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto v_all_ones = hn::Set(d, static_cast<lane_type>(static_cast<lane_type>(-1)));
    const auto v_zero = hn::Zero(d);
    auto sa = vec_stream<decltype(d), lane_type, stream_width>::pack(v_all_ones, v_zero);

    auto ms = MaskFromVec(sa);
    static_assert(is_mask_stream_v<decltype(ms)>);
    // First slot has all-true (lane bits = -1), second is all-false.
    ASSERT(!hn::AllFalse(d, ms[0]));
    ASSERT(hn::AllFalse(d, ms[1]));
}

TEMPLATE_TEST_CASE("vec_stream StoreStream",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint16_t, std::uint32_t, std::uint64_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    HWY_ALIGN std::array<lane_type, hn::MaxLanes(d)> a{};
    HWY_ALIGN std::array<lane_type, hn::MaxLanes(d)> b{};

    const lane_type val_a = static_cast<lane_type>(0xAA);
    const lane_type val_b = static_cast<lane_type>(0xBB);
    auto s = vec_stream<decltype(d), lane_type, stream_width>::pack(
                hn::Set(d, val_a), hn::Set(d, val_b));

    StoreStream(s, d, std::array<lane_type*, stream_width>{a.data(), b.data()});

    for (std::size_t i = 0; i < hn::Lanes(d); ++i) {
        ASSERT(a[i] == val_a);
        ASSERT(b[i] == val_b);
    }
}

TEMPLATE_TEST_CASE("vec_stream AllFalsePerStream",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint32_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 2;

    const auto m_false = hn::MaskFalse(d);
    const auto m_first = hn::FirstN(d, 1);     // at least one lane true
    auto ms = mask_stream<decltype(d), stream_width>::pack(m_false, m_first);

    const auto per = AllFalsePerStream(d, ms);
    ASSERT(per[0] == true);
    ASSERT(per[1] == false);
}

TEMPLATE_TEST_CASE("vec_stream stream_width==1 collapses cleanly",
                   "[component:bioseq][layer:unit][feature:simd_primitives][resource:highway]",
                   std::uint32_t) {
    using lane_type = TestType;
    const hn::ScalableTag<lane_type> d;
    constexpr std::size_t stream_width = 1;

    const auto va = hn::Set(d, static_cast<lane_type>(7));
    const auto vb = hn::Set(d, static_cast<lane_type>(11));
    auto sa = vec_stream<decltype(d), lane_type, stream_width>::pack(va);
    auto sb = vec_stream<decltype(d), lane_type, stream_width>::pack(vb);

    auto rs = Add(sa, sb);
    static_assert(decltype(rs)::stream_width == 1);
    const auto got = hwy_to_vector<decltype(d), lane_type>(d, rs[0]);
    const auto want = hwy_to_vector<decltype(d), lane_type>(d, hn::Add(va, vb));
    ASSERT(got == want);
}

// =============================================================================
// Trivially-copyable / destructible static_asserts — module-load-time check.
// =============================================================================

static_assert(std::is_trivially_copyable_v<vec_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>,
              "vec_stream must be trivially copyable");
static_assert(std::is_trivially_destructible_v<vec_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>,
              "vec_stream must be trivially destructible");
static_assert(std::is_trivially_copyable_v<mask_stream<hn::ScalableTag<std::uint32_t>, 2>>,
              "mask_stream must be trivially copyable");

// is_*_stream_v traits sanity.
static_assert(is_vec_stream_v<vec_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>);
static_assert(!is_vec_stream_v<hn::Vec<hn::ScalableTag<std::uint32_t>>>);
static_assert(is_mask_stream_v<mask_stream<hn::ScalableTag<std::uint32_t>, 2>>);
static_assert(!is_mask_stream_v<hn::Mask<hn::ScalableTag<std::uint32_t>>>);
static_assert(is_any_stream_v<vec_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>>);
static_assert(is_any_stream_v<mask_stream<hn::ScalableTag<std::uint32_t>, 2>>);
static_assert(any_arg_is_stream<vec_stream<hn::ScalableTag<std::uint32_t>, std::uint32_t, 2>, int>);
static_assert(!any_arg_is_stream<int, double>);

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
