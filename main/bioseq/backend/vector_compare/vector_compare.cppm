/*-------------------------------------------------------------------------
 *
 * vector_compare/vector_compare.cppm   (module bioseq_column.vector_compare)
 *
 * Domain-N (NO_POSTGRES) element-wise comparison kernel for the columnar
 * vectorised executor. This is the algorithm that used to live behind the
 * _BUILD_CMP_OP_INT codegen macro in Domain P; it is now expressed once as a
 * width-templated kernel plus the concept the Domain-P binding
 * (bioseq_column_vector_compare_pg) must satisfy. No Datum, no VectorColumn,
 * no PostgreSQL header — alphabet/Datum-free by construction.
 *
 * The comparison loop is a plain, contiguous, dependency-free loop that clang
 * -O3 auto-vectorises; it is the qual-filter path, not a hot inner loop, and
 * the natural C++ promotion in `values[i] <op> k` reproduces the original
 * macro's cross-width semantics EXACTLY (LTYPE value vs RTYPE constant, both
 * promoted to their common type) — without the fragile Highway mask-narrowing
 * that mixed-width -> bool[] packing would require. The boolean mask combiners
 * (mask_and / mask_or) are same-width uint8 ops and stay genuine Highway.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common_simd.h"   // <hwy/highway.h> + friends (tolerated header), for mask_and/mask_or

#include <cstdint>
#include <span>
#include <utility>

export module bioseq.column.vector_compare;

static_assert(__cplusplus >= 202302L, "bioseq_column.vector_compare requires C++23-or-later");

export namespace bioseq::column::vec {

// The six SQL comparison operators, expressed once (replaces the 84 generated
// functions' bodies).
enum class CmpOp : std::uint8_t { eq, ne, gt, lt, le, ge };

// Apply one operator; A and B are the column-value and constant element types.
// `a <op> b` performs the usual arithmetic conversions, so a narrow column vs a
// wider constant (e.g. int2 column vs int4 const) compares at the common type —
// exactly the semantics of the original PG cross-width operators.
template <CmpOp Op, class A, class B>
[[gnu::always_inline]] constexpr bool cmp_apply(A a, B b) noexcept {
    if constexpr (Op == CmpOp::eq) return a == b;
    else if constexpr (Op == CmpOp::ne) return a != b;
    else if constexpr (Op == CmpOp::gt) return a >  b;
    else if constexpr (Op == CmpOp::lt) return a <  b;
    else if constexpr (Op == CmpOp::le) return a <= b;
    else                                return a >= b;
}

// The LOAD-BEARING boundary concept (DOC0321 §7.1): Domain N DEFINES it; the
// Domain-P binding SUPPLIES a concrete type that satisfies it. The kernel writes
// its result THROUGH the sink, so the concept genuinely constrains the binding —
// a drift breaks the build at the binding's static_assert. allocate(n) is called
// ONCE per batch to size the two result buffers; value_data()/null_data() then
// hand the kernel the raw, writable bool[n] bases (the keep mask and the result
// null mask).
//
// IMPORTANT (DOC0322 §6.4): the accessors return a *plain pointer*, never an
// aggregate (no pair/span by value). Returning a `std::pair<std::span,std::span>`
// across this Domain-P -> Domain-N module-interface boundary is mis-marshalled by
// clang's C++20 modules ABI (the sret aggregate comes back pointer-shifted, so
// the kernel writes the mask at a +2 offset and the first rows read as 0). A
// register-returned `bool*` is ABI-bulletproof.
template <class S>
concept is_compare_result_sink = requires(S s, std::uint32_t n) {
    { s.allocate(n) };
    { s.value_data() } -> std::same_as<bool*>;
    { s.null_data() }  -> std::same_as<bool*>;
};

}  // namespace bioseq::column::vec

/* ---- Highway implementation of the same-width bool mask combiners ---- */
HWY_BEFORE_NAMESPACE();
namespace bioseq::column::vec::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// lhs[i] &= rhs[i]   (operates on bool[] reinterpreted as uint8[])
HWY_INLINE void mask_and_impl(std::span<bool> lhs, std::span<const bool> rhs) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t N = hn::Lanes(d);
    const std::size_t n = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    std::uint8_t* HWY_RESTRICT lp = reinterpret_cast<std::uint8_t*>(lhs.data());
    const std::uint8_t* HWY_RESTRICT rp = reinterpret_cast<const std::uint8_t*>(rhs.data());
    std::size_t i = 0;
    for (; i + N <= n; i += N)
        hn::StoreU(hn::And(hn::LoadU(d, lp + i), hn::LoadU(d, rp + i)), d, lp + i);
    if (i < n) {
        const std::size_t rem = n - i;
        hn::StoreN(hn::And(hn::LoadN(d, lp + i, rem), hn::LoadN(d, rp + i, rem)), d, lp + i, rem);
    }
}

// lhs[i] |= rhs[i]
HWY_INLINE void mask_or_impl(std::span<bool> lhs, std::span<const bool> rhs) {
    const hn::ScalableTag<std::uint8_t> d;
    const std::size_t N = hn::Lanes(d);
    const std::size_t n = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    std::uint8_t* HWY_RESTRICT lp = reinterpret_cast<std::uint8_t*>(lhs.data());
    const std::uint8_t* HWY_RESTRICT rp = reinterpret_cast<const std::uint8_t*>(rhs.data());
    std::size_t i = 0;
    for (; i + N <= n; i += N)
        hn::StoreU(hn::Or(hn::LoadU(d, lp + i), hn::LoadU(d, rp + i)), d, lp + i);
    if (i < n) {
        const std::size_t rem = n - i;
        hn::StoreN(hn::Or(hn::LoadN(d, lp + i, rem), hn::LoadN(d, rp + i, rem)), d, lp + i, rem);
    }
}

}  // namespace bioseq::column::vec::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

/* ---- Public kernel API (non-HWY-namespaced) ---- */
export namespace bioseq::column::vec {

// result[i] = !nulls[i] && (values[i] OP k);  result_nulls[i] = nulls[i].
// ValueT = column-value element type, ConstT = constant element type. The
// comparison uses cmp_apply (natural C++ promotion) so cross-width combos are
// exact. allocate() runs ONCE per batch; the loop auto-vectorises at -O3.
// NOTE (DOC0322 §6.4): parameters are plain pointer + count, NOT std::span.
// Passing a std::span by value across this Domain-P -> Domain-N module-interface
// boundary is mis-marshalled by clang's C++20-modules ABI (the {ptr,size} pair
// comes back with the data pointer shifted, so the kernel reads vp[i] from the
// wrong offset and the leading rows of the mask read as 0). Pointer + size args
// are register-passed and ABI-bulletproof.
template <CmpOp Op, class ValueT, class ConstT, is_compare_result_sink Sink>
void compare_column(const ValueT* vp, const bool* np, std::uint32_t n,
                    ConstT k, Sink& sink) {
    sink.allocate(n);
    bool* op = sink.value_data();   // raw bool[n] keep-mask base
    bool* onp = sink.null_data();   // raw bool[n] result-null base
    for (std::uint32_t i = 0; i < n; ++i) {
        const bool is_null = np[i];
        onp[i] = is_null;
        op[i] = is_null ? false : cmp_apply<Op>(vp[i], k);
    }
}

inline void mask_and(std::span<bool> lhs, std::span<const bool> rhs) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::mask_and_impl(lhs, rhs);
}

inline void mask_or(std::span<bool> lhs, std::span<const bool> rhs) noexcept {
    bioseq::column::vec::HWY_NAMESPACE::mask_or_impl(lhs, rhs);
}

}  // namespace bioseq::column::vec
