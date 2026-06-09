#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

import bioseq_simd.utils;
import bioseq_simd.vector_stream;
import mlogging;
import testing;

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

template <class D, size_t W, typename Lifted, typename HnOp>
void check_binary_vec_op(D d, Lifted lifted, HnOp hn_op, const char* name) {
    using ST = hn::TFromD<D>;
    const auto va = hn::Set(d, static_cast<ST>(0xAA));
    const auto vb = hn::Set(d, static_cast<ST>(0x55));
    const auto vc = hn::Set(d, static_cast<ST>(0x12));

    auto sa = vec_stream<D, ST, W>::pack(va, vb);
    auto sb = vec_stream<D, ST, W>::pack(vb, vc);

    {
        auto rs = lifted(sa, sb);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<D, ST>(d, rs[k]);
            const auto want = hwy_to_vector<D, ST>(d, hn_op(sa[k], sb[k]));
            ASSERT(got == want);
        }
    }
    {
        auto rs = lifted(sa, vc);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<D, ST>(d, rs[k]);
            const auto want = hwy_to_vector<D, ST>(d, hn_op(sa[k], vc));
            ASSERT(got == want);
        }
    }
    {
        auto rs = lifted(va, sb);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<D, ST>(d, rs[k]);
            const auto want = hwy_to_vector<D, ST>(d, hn_op(va, sb[k]));
            ASSERT(got == want);
        }
    }
    bioseq_log<DBG>("[vec_stream binary] op={} OK", name);
}

// Comparison ops: result is a mask_stream. Compare via VecFromMask round-trip.
template <class D, size_t W, typename Lifted, typename HnOp>
void check_binary_cmp_op(D d, Lifted lifted, HnOp hn_op, const char* name) {
    using ST = hn::TFromD<D>;
    const auto va = hn::Set(d, static_cast<ST>(5));
    const auto vb = hn::Set(d, static_cast<ST>(10));

    auto sa = vec_stream<D, ST, W>::pack(va, vb);
    auto sb = vec_stream<D, ST, W>::pack(vb, va);

    auto ms = lifted(sa, sb);
    for (size_t k = 0; k < W; ++k) {
        const auto got = hwy_to_vector<D, ST>(d, hn::VecFromMask(d, ms[k]));
        const auto want = hwy_to_vector<D, ST>(d, hn::VecFromMask(d, hn_op(sa[k], sb[k])));
        ASSERT(got == want);
    }
    bioseq_log<DBG>("[vec_stream cmp] op={} OK", name);
}

// =============================================================================
// Tests — one TEST_CASE per op family for clean failure isolation.
// =============================================================================

TEMPLATE_TEST_CASE("vec_stream binary arithmetic equivalence",
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;

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
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;

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

TEMPLATE_TEST_CASE("mask_stream AndNot vs plain mask broadcast — Phase 3 path",
                   "[vec_stream][mask_stream]", uint16_t, uint32_t, uint64_t) {
    // Phase-3 consumer path: the two-stack collector palindrome branch
    // computes `AndNot(palindrome /*mask_stream<D,1>*/, live_mask /*mask_t*/)`
    // — a (mask_stream, mask_t) pair that the Phase-2 module routes
    // through `BIOSEQ_VS_BINARY_WRAPPER(AndNot)` via `lift_op` projecting
    // the plain mask as broadcast. We verify byte-equality against
    // direct `hn::AndNot` per slot to lock the contract before the
    // Phase-3 EXPECTED_PAIRS gate fires.
    using ST = TestType;
    const hn::ScalableTag<ST> d;

    const auto pal_a = hn::Eq(hn::Set(d, static_cast<ST>(5)),
                              hn::Set(d, static_cast<ST>(5)));   // all-true
    const auto pal_b = hn::Ne(hn::Set(d, static_cast<ST>(5)),
                              hn::Set(d, static_cast<ST>(5)));   // all-false
    const auto live = hn::Eq(hn::Set(d, static_cast<ST>(7)),
                             hn::Set(d, static_cast<ST>(7)));    // all-true

    auto pal_stream = mask_stream<decltype(d), 2>::pack(pal_a, pal_b);

    // Lifted: mask_stream ⊕ plain mask
    auto result_stream = AndNot(pal_stream, live);
    for (size_t s = 0; s < 2; ++s) {
        const auto got  = hwy_to_vector<decltype(d), ST>(d,
            hn::VecFromMask(d, result_stream[s]));
        const auto want = hwy_to_vector<decltype(d), ST>(d,
            hn::VecFromMask(d, hn::AndNot(pal_stream[s], live)));
        ASSERT(got == want);
    }
    bioseq_log<DBG>("[mask_stream AndNot+broadcast] OK");
}

TEMPLATE_TEST_CASE("vec_stream binary comparisons",
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;

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
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto va = hn::Set(d, static_cast<ST>(0x35));
    const auto vb = hn::Set(d, static_cast<ST>(0x91));
    auto sa = vec_stream<decltype(d), ST, W>::pack(va, vb);

    {
        auto rs = ShiftLeft<3>(sa);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d, hn::ShiftLeft<3>(sa[k]));
            ASSERT(got == want);
        }
    }
    {
        auto rs = ShiftRight<2>(sa);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d, hn::ShiftRight<2>(sa[k]));
            ASSERT(got == want);
        }
    }
}

TEMPLATE_TEST_CASE("vec_stream runtime same-amount shifts",
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto va = hn::Set(d, static_cast<ST>(0x35));
    const auto vb = hn::Set(d, static_cast<ST>(0x91));
    auto sa = vec_stream<decltype(d), ST, W>::pack(va, vb);
    const int amt = 3;

    {
        auto rs = ShiftLeftSame(sa, amt);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d, hn::ShiftLeftSame(sa[k], amt));
            ASSERT(got == want);
        }
    }
    {
        auto rs = ShiftRightSame(sa, amt);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d, hn::ShiftRightSame(sa[k], amt));
            ASSERT(got == want);
        }
    }
}

TEMPLATE_TEST_CASE("vec_stream IfThenElse — every argument shape",
                   "[vec_stream][ite]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto m_a = hn::Lt(hn::Set(d, ST{1}), hn::Set(d, ST{2}));   // all-true
    const auto m_b = hn::Lt(hn::Set(d, ST{2}), hn::Set(d, ST{1}));   // all-false
    auto ms = mask_stream<decltype(d), W>::pack(m_a, m_b);

    const auto va = hn::Set(d, static_cast<ST>(0xAA));
    const auto vb = hn::Set(d, static_cast<ST>(0x55));
    auto sa = vec_stream<decltype(d), ST, W>::pack(va, vb);
    auto sb = vec_stream<decltype(d), ST, W>::pack(vb, va);

    // (mask_stream, vec_stream, vec_stream)
    {
        auto rs = IfThenElse(ms, sa, sb);
        static_assert(is_vec_stream_v<decltype(rs)>);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d,
                hn::IfThenElse(ms[k], sa[k], sb[k]));
            ASSERT(got == want);
        }
    }

    // (mask_stream, vec_stream, plain_vec)
    {
        auto rs = IfThenElse(ms, sa, vb);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d,
                hn::IfThenElse(ms[k], sa[k], vb));
            ASSERT(got == want);
        }
    }

    // (mask_stream, plain_vec, vec_stream)
    {
        auto rs = IfThenElse(ms, va, sb);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d,
                hn::IfThenElse(ms[k], va, sb[k]));
            ASSERT(got == want);
        }
    }

    // (mask_stream, plain_vec, plain_vec) — the case the v1 plan deduced wrong.
    {
        auto rs = IfThenElse(ms, va, vb);
        static_assert(is_vec_stream_v<decltype(rs)>,
                      "IfThenElse(mask_stream, vec, vec) must produce vec_stream");
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d,
                hn::IfThenElse(ms[k], va, vb));
            ASSERT(got == want);
        }
    }

    // (plain_mask, vec_stream, vec_stream) — broadcast mask
    {
        auto rs = IfThenElse(m_a, sa, sb);
        for (size_t k = 0; k < W; ++k) {
            const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
            const auto want = hwy_to_vector<decltype(d), ST>(d,
                hn::IfThenElse(m_a, sa[k], sb[k]));
            ASSERT(got == want);
        }
    }

    bioseq_log<DBG>("[vec_stream IfThenElse] all 5 shapes OK");
}

TEMPLATE_TEST_CASE("vec_stream static factories",
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    auto z = vec_stream<decltype(d), ST, W>::splat_zero(d);
    const auto zero_lanes = hwy_to_vector<decltype(d), ST>(d, hn::Zero(d));
    for (size_t k = 0; k < W; ++k) {
        const auto got = hwy_to_vector<decltype(d), ST>(d, z[k]);
        ASSERT(got == zero_lanes);
    }

    const ST val = static_cast<ST>(0x12);
    auto s = vec_stream<decltype(d), ST, W>::splat(d, val);
    const auto val_lanes = hwy_to_vector<decltype(d), ST>(d, hn::Set(d, val));
    for (size_t k = 0; k < W; ++k) {
        const auto got = hwy_to_vector<decltype(d), ST>(d, s[k]);
        ASSERT(got == val_lanes);
    }
}

TEMPLATE_TEST_CASE("mask_stream static factory splat_false",
                   "[vec_stream][mask]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    auto m = mask_stream<decltype(d), W>::splat_false(d);
    for (size_t k = 0; k < W; ++k) {
        ASSERT(hn::AllFalse(d, m[k]));
    }
}

TEMPLATE_TEST_CASE("vec_stream Not / Neg / Abs",
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto va = hn::Set(d, static_cast<ST>(0x35));
    const auto vb = hn::Set(d, static_cast<ST>(0x91));
    auto sa = vec_stream<decltype(d), ST, W>::pack(va, vb);

    auto rs = Not(sa);
    for (size_t k = 0; k < W; ++k) {
        const auto got = hwy_to_vector<decltype(d), ST>(d, rs[k]);
        const auto want = hwy_to_vector<decltype(d), ST>(d, hn::Not(sa[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vec_stream BitCast across types",
                   "[vec_stream]", uint32_t) {
    const hn::ScalableTag<uint32_t> d_in;
    const hn::ScalableTag<uint8_t> d_out;
    constexpr size_t W = 2;

    const auto va = hn::Set(d_in, uint32_t{0xDEADBEEF});
    const auto vb = hn::Set(d_in, uint32_t{0x12345678});
    auto sa = vec_stream<decltype(d_in), uint32_t, W>::pack(va, vb);

    auto rs = BitCast(d_out, sa);
    static_assert(is_vec_stream_v<decltype(rs)>);
    static_assert(std::is_same_v<typename decltype(rs)::tag_t,
                                   std::remove_cvref_t<decltype(d_out)>>);

    for (size_t k = 0; k < W; ++k) {
        const auto got = hwy_to_vector<decltype(d_out), uint8_t>(d_out, rs[k]);
        const auto want = hwy_to_vector<decltype(d_out), uint8_t>(d_out,
            hn::BitCast(d_out, sa[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vec_stream VecFromMask round-trip",
                   "[vec_stream][mask]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto m_a = hn::Lt(hn::Set(d, ST{1}), hn::Set(d, ST{2}));   // all-true
    const auto m_b = hn::Lt(hn::Set(d, ST{2}), hn::Set(d, ST{1}));   // all-false
    auto ms = mask_stream<decltype(d), W>::pack(m_a, m_b);

    auto vs = VecFromMask(d, ms);
    static_assert(is_vec_stream_v<decltype(vs)>);
    for (size_t k = 0; k < W; ++k) {
        const auto got = hwy_to_vector<decltype(d), ST>(d, vs[k]);
        const auto want = hwy_to_vector<decltype(d), ST>(d, hn::VecFromMask(d, ms[k]));
        ASSERT(got == want);
    }
}

TEMPLATE_TEST_CASE("vec_stream MaskFromVec round-trip",
                   "[vec_stream][mask]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto v_all_ones = hn::Set(d, static_cast<ST>(static_cast<ST>(-1)));
    const auto v_zero = hn::Zero(d);
    auto sa = vec_stream<decltype(d), ST, W>::pack(v_all_ones, v_zero);

    auto ms = MaskFromVec(sa);
    static_assert(is_mask_stream_v<decltype(ms)>);
    // First slot has all-true (lane bits = -1), second is all-false.
    ASSERT(!hn::AllFalse(d, ms[0]));
    ASSERT(hn::AllFalse(d, ms[1]));
}

TEMPLATE_TEST_CASE("vec_stream StoreStream",
                   "[vec_stream]", uint16_t, uint32_t, uint64_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    HWY_ALIGN std::array<ST, hn::MaxLanes(d)> a{};
    HWY_ALIGN std::array<ST, hn::MaxLanes(d)> b{};

    const ST val_a = static_cast<ST>(0xAA);
    const ST val_b = static_cast<ST>(0xBB);
    auto s = vec_stream<decltype(d), ST, W>::pack(
                hn::Set(d, val_a), hn::Set(d, val_b));

    StoreStream(s, d, std::array<ST*, W>{a.data(), b.data()});

    for (size_t i = 0; i < hn::Lanes(d); ++i) {
        ASSERT(a[i] == val_a);
        ASSERT(b[i] == val_b);
    }
}

TEMPLATE_TEST_CASE("vec_stream AllFalsePerStream",
                   "[vec_stream][mask]", uint32_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 2;

    const auto m_false = hn::MaskFalse(d);
    const auto m_first = hn::FirstN(d, 1);     // at least one lane true
    auto ms = mask_stream<decltype(d), W>::pack(m_false, m_first);

    const auto per = AllFalsePerStream(d, ms);
    ASSERT(per[0] == true);
    ASSERT(per[1] == false);
}

TEMPLATE_TEST_CASE("vec_stream W==1 collapses cleanly",
                   "[vec_stream]", uint32_t) {
    using ST = TestType;
    const hn::ScalableTag<ST> d;
    constexpr size_t W = 1;

    const auto va = hn::Set(d, static_cast<ST>(7));
    const auto vb = hn::Set(d, static_cast<ST>(11));
    auto sa = vec_stream<decltype(d), ST, W>::pack(va);
    auto sb = vec_stream<decltype(d), ST, W>::pack(vb);

    auto rs = Add(sa, sb);
    static_assert(decltype(rs)::stream_width == 1);
    const auto got = hwy_to_vector<decltype(d), ST>(d, rs[0]);
    const auto want = hwy_to_vector<decltype(d), ST>(d, hn::Add(va, vb));
    ASSERT(got == want);
}

// =============================================================================
// Trivially-copyable / destructible static_asserts — module-load-time check.
// =============================================================================

static_assert(std::is_trivially_copyable_v<vec_stream<hn::ScalableTag<uint32_t>, uint32_t, 2>>,
              "vec_stream must be trivially copyable");
static_assert(std::is_trivially_destructible_v<vec_stream<hn::ScalableTag<uint32_t>, uint32_t, 2>>,
              "vec_stream must be trivially destructible");
static_assert(std::is_trivially_copyable_v<mask_stream<hn::ScalableTag<uint32_t>, 2>>,
              "mask_stream must be trivially copyable");

// is_*_stream_v traits sanity.
static_assert(is_vec_stream_v<vec_stream<hn::ScalableTag<uint32_t>, uint32_t, 2>>);
static_assert(!is_vec_stream_v<hn::Vec<hn::ScalableTag<uint32_t>>>);
static_assert(is_mask_stream_v<mask_stream<hn::ScalableTag<uint32_t>, 2>>);
static_assert(!is_mask_stream_v<hn::Mask<hn::ScalableTag<uint32_t>>>);
static_assert(is_any_stream_v<vec_stream<hn::ScalableTag<uint32_t>, uint32_t, 2>>);
static_assert(is_any_stream_v<mask_stream<hn::ScalableTag<uint32_t>, 2>>);
static_assert(any_arg_is_stream<vec_stream<hn::ScalableTag<uint32_t>, uint32_t, 2>, int>);
static_assert(!any_arg_is_stream<int, double>);

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
