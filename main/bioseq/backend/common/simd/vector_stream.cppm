module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <array>
#include <type_traits>
#include <utility>

#if HWY_TARGET == HWY_RVV || HWY_TARGET == HWY_SVE || HWY_TARGET == HWY_SVE2
#  error "bioseq_simd.vector_stream: vectors are sizeless on RVV/SVE; this module is only enabled for fixed-vector targets (NEON, SSE/AVX, AVX-512, SCALAR)."
#endif

export module bioseq_simd.vector_stream;

import mlogging;
import bioseq_simd.utils;     // for_each_index<W>

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// Bundle types
// ============================================================================
//
// vec_stream<D, ST, W> is a bundle of W hn::Vec<D> instances. It carries the
// tag D and the storage_unit ST so that lifted operations can deduce both at
// compile time without needing the user to spell them out.
//
// mask_stream<D, W> is the analogous bundle of hn::Mask<D>. We keep masks as
// their own type because on AVX-512 hn::Mask<D> is __mmask16/__mmask32 (a
// scalar predicate register) and converting to/from a vector via VecFromMask
// is a non-trivial cost — see plan §3.3.

template <size_t W> concept stream_width_ok = (W >= 1 && W <= 4);

// Forward declarations carry the same `requires` clause as definitions.
template <class D, typename ST, size_t W>
requires (stream_width_ok<W>)
struct vec_stream;

template <class D, size_t W>
requires (stream_width_ok<W>)
struct mask_stream;

template <typename T> struct is_vec_stream : std::false_type {};
template <class D, typename ST, size_t W>
struct is_vec_stream<vec_stream<D, ST, W>> : std::true_type {};
template <typename T>
inline constexpr bool is_vec_stream_v = is_vec_stream<std::remove_cvref_t<T>>::value;

template <typename T> struct is_mask_stream : std::false_type {};
template <class D, size_t W>
struct is_mask_stream<mask_stream<D, W>> : std::true_type {};
template <typename T>
inline constexpr bool is_mask_stream_v = is_mask_stream<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool is_any_stream_v =
    is_vec_stream_v<T> || is_mask_stream_v<T>;

template <typename... Args>
concept any_arg_is_stream = (is_any_stream_v<Args> || ... || false);

// ----------------------------------------------------------------------------
// vec_stream
// ----------------------------------------------------------------------------

template <class D, typename ST, size_t W>
requires (stream_width_ok<W>)
struct
#if defined(__clang__)
[[clang::trivial_abi]]
#endif
vec_stream {
    // Strip cv from D so the type is identical regardless of whether the
    // user wrote vec_stream<decltype(const_d), ...> or
    // vec_stream<hn::ScalableTag<T>, ...>. Highway's Vec<D> alias does
    // `decltype(Zero(D()))` and rejects const-qualified D — see hwy/ops/
    // generic_ops-inl.h:47.
    using tag_t = std::remove_cvref_t<D>;
    using stored_type = ST;
    using vec_t = hn::Vec<tag_t>;
    static constexpr size_t stream_width = W;

private:
    // No HWY_ALIGN — `vec_t` (Highway Vec<D>) carries its own alignment via
    // the underlying SIMD ABI. Adding `alignas(…)` on the wrapping std::array
    // can inhibit SROA: the optimizer treats the bundle as a memory object
    // rather than scalar-replacing per-slot. The named-scalar layout from
    // Phase 1 worked because each `vec_t` was an independent local.
    std::array<vec_t, W> _data;

    // Explicit private constructor used by pack/splat factories. Since _data
    // is private, the type is non-aggregate and brace-init `vec_stream{...}`
    // would otherwise be ill-formed.
    explicit HWY_INLINE constexpr vec_stream(std::array<vec_t, W> d) noexcept
        : _data(d) {}

public:
    HWY_INLINE constexpr vec_stream() noexcept = default;

    [[nodiscard]] constexpr vec_t&       operator[](size_t s)       noexcept { return _data[s]; }
    [[nodiscard]] constexpr const vec_t& operator[](size_t s) const noexcept { return _data[s]; }

    [[nodiscard]] constexpr std::array<vec_t, W>&       data()       noexcept { return _data; }
    [[nodiscard]] constexpr const std::array<vec_t, W>& data() const noexcept { return _data; }

    /// Type-safe pack — exactly W vec_t arguments. Direct aggregate-init of
    /// the underlying std::array; the only point of construction. NRVO-friendly.
    template <typename... Vs>
    requires (sizeof...(Vs) == W && (std::is_convertible_v<Vs, vec_t> && ...))
    [[nodiscard]] static HWY_INLINE constexpr vec_stream pack(Vs... vs) noexcept {
        return vec_stream{ std::array<vec_t, W>{ static_cast<vec_t>(vs)... } };
    }

    /// All-slots-equal factory.
    /// Direct construction via array-initializer + parameter-pack expansion.
    /// No "default-construct + per-slot assign" cycle — the std::array is
    /// fully initialized in one expression so the compiler doesn't have to
    /// reason about partial writes.
    [[nodiscard]] static HWY_INLINE vec_stream splat(tag_t d, ST value) noexcept {
        return splat_impl(hn::Set(d, value),
            std::make_index_sequence<W>{});
    }

    [[nodiscard]] static HWY_INLINE vec_stream splat_zero(tag_t d) noexcept {
        return splat(d, ST{0});
    }

private:
    /// Implementation helper for `splat`. Uses parameter-pack expansion
    /// `((void)S, v)...` to repeat `v` W times in the `pack` argument list.
    template <size_t... S>
    [[nodiscard]] static HWY_INLINE vec_stream
    splat_impl(vec_t v, std::index_sequence<S...>) noexcept {
        return pack(((void)S, v)...);
    }
public:

    // Hidden-friend operators. These exist solely so Highway's generic-V
    // fallbacks `Add(V, V) { return a + b; }`, `Sub(V, V) { return a - b; }`,
    // etc. (declared as `template<class V>` in
    // hwy/ops/generic_ops-inl.h:8131+) compile when V is deduced to vec_stream.
    // Without these operators, Highway's Add(V, V) would fail in its body,
    // *and* it wins partial ordering against our generic free-function Add(A, B)
    // because V=V is more specific than A, B. With these operators, Highway's
    // generic Add cleanly delegates back to our lift_op machinery via the
    // operator forward.
    //
    // The operators themselves are simple forwards to the same lift_op the
    // free-function wrappers use, so behaviour is identical.
    //
    // Hidden friends are found only via ADL on vec_stream args, so they don't
    // pollute the surrounding namespace.

    // Hidden-friend operators. Take vec_stream BY VALUE — for W ∈ {1, 2}
    // the bundle is sizeof(vec_t) * W = 16 or 32 bytes, which the M1 NEON
    // ABI passes as an HVA (homogeneous vector aggregate) in V0/V1
    // registers. By-reference passing forces an address escape that
    // can prevent SROA on the caller's side. The pack-expansion form
    // gives the optimizer a single point of construction.

#define BIOSEQ_VS_HIDDEN_FRIEND_OP(OPSYM, HNNAME)                                  \
    [[nodiscard]] friend HWY_INLINE auto operator OPSYM(                            \
        vec_stream a, vec_stream b) noexcept {                                      \
        return op_pack_impl_##HNNAME(a, b, std::make_index_sequence<W>{});          \
    }                                                                               \
    template <size_t... S>                                                          \
    [[nodiscard, gnu::always_inline]] static HWY_INLINE                             \
    vec_stream op_pack_impl_##HNNAME(                                               \
        vec_stream a, vec_stream b, std::index_sequence<S...>) noexcept {           \
        return pack(hn::HNNAME(a[S], b[S])...);                                     \
    }

    BIOSEQ_VS_HIDDEN_FRIEND_OP(+,  Add)
    BIOSEQ_VS_HIDDEN_FRIEND_OP(-,  Sub)
    BIOSEQ_VS_HIDDEN_FRIEND_OP(*,  Mul)

#undef BIOSEQ_VS_HIDDEN_FRIEND_OP

    // Comparison operators return a mask_stream<tag_t, W>.
#define BIOSEQ_VS_HIDDEN_FRIEND_CMP(OPSYM, HNNAME)                                 \
    [[nodiscard]] friend HWY_INLINE auto operator OPSYM(                            \
        vec_stream a, vec_stream b) noexcept {                                      \
        return cmp_pack_impl_##HNNAME(a, b, std::make_index_sequence<W>{});         \
    }                                                                               \
    template <size_t... S>                                                          \
    [[nodiscard, gnu::always_inline]] static HWY_INLINE                             \
    mask_stream<tag_t, W> cmp_pack_impl_##HNNAME(                                   \
        vec_stream a, vec_stream b, std::index_sequence<S...>) noexcept {           \
        return mask_stream<tag_t, W>::pack(hn::HNNAME(a[S], b[S])...);              \
    }

    BIOSEQ_VS_HIDDEN_FRIEND_CMP(==, Eq)
    BIOSEQ_VS_HIDDEN_FRIEND_CMP(!=, Ne)
    BIOSEQ_VS_HIDDEN_FRIEND_CMP(<,  Lt)
    BIOSEQ_VS_HIDDEN_FRIEND_CMP(>,  Gt)
    BIOSEQ_VS_HIDDEN_FRIEND_CMP(<=, Le)
    BIOSEQ_VS_HIDDEN_FRIEND_CMP(>=, Ge)

#undef BIOSEQ_VS_HIDDEN_FRIEND_CMP

};

// ----------------------------------------------------------------------------
// mask_stream
// ----------------------------------------------------------------------------

template <class D, size_t W>
requires (stream_width_ok<W>)
struct
#if defined(__clang__)
[[clang::trivial_abi]]
#endif
mask_stream {
    using tag_t = std::remove_cvref_t<D>;
    using mask_t = hn::Mask<tag_t>;
    static constexpr size_t stream_width = W;

private:
    // See comment on vec_stream::_data — same SROA rationale.
    std::array<mask_t, W> _data;

    explicit HWY_INLINE constexpr mask_stream(std::array<mask_t, W> d) noexcept
        : _data(d) {}

public:
    HWY_INLINE constexpr mask_stream() noexcept = default;

    [[nodiscard]] constexpr mask_t&       operator[](size_t s)       noexcept { return _data[s]; }
    [[nodiscard]] constexpr const mask_t& operator[](size_t s) const noexcept { return _data[s]; }

    [[nodiscard]] constexpr std::array<mask_t, W>&       data()       noexcept { return _data; }
    [[nodiscard]] constexpr const std::array<mask_t, W>& data() const noexcept { return _data; }

    template <typename... Ms>
    requires (sizeof...(Ms) == W && (std::is_convertible_v<Ms, mask_t> && ...))
    [[nodiscard]] static HWY_INLINE constexpr mask_stream pack(Ms... ms) noexcept {
        return mask_stream{ std::array<mask_t, W>{ static_cast<mask_t>(ms)... } };
    }

    [[nodiscard]] static HWY_INLINE mask_stream splat_false(tag_t d) noexcept {
        return splat_false_impl(hn::MaskFalse(d),
            std::make_index_sequence<W>{});
    }

private:
    template <size_t... S>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE mask_stream
    splat_false_impl(mask_t m, std::index_sequence<S...>) noexcept {
        return pack(((void)S, m)...);
    }
public:
};

// ============================================================================
// Lifting machinery
// ============================================================================

namespace vec_stream_internal {

/// Pick out a per-slot view of an argument for use inside a lifted op:
///   - If arg is a stream, return arg[S] (by reference into the stream).
///   - Otherwise, return arg verbatim (broadcast — same lvalue every slot).
///
/// project<S> takes Arg by lvalue-ref and returns by reference; this is
/// deliberate. A forwarding-ref + std::forward variant would move-from a
/// non-stream rvalue argument on the first slot iteration and access a
/// moved-from object on subsequent iterations. The lambda inside lift_op
/// captures arguments by reference and projects them W times.
template <size_t S, typename Arg>
[[gnu::always_inline]] HWY_INLINE
constexpr decltype(auto) project(Arg& arg) {
    if constexpr (is_any_stream_v<Arg>) {
        return arg[S];
    } else {
        return (arg);    // parens force lvalue-ref return
    }
}

// Width / tag / stored-type deduction over an argument pack.
//
// The naive ternary form `is_any_stream_v<T> ? T::stream_width : recurse`
// fails because `T::stream_width` is type-checked even when `T` is not a
// stream. We use constrained partial specializations so the member access
// only happens on the "is-a-stream" branch.

// --- Width ------------------------------------------------------------------

// Per-arg helper: evaluates to T's stream_width when T is a stream, else 0.
template <typename T>
inline constexpr size_t stream_width_of_v = 0;

template <typename T>
requires (is_any_stream_v<T>)
inline constexpr size_t stream_width_of_v<T> =
    std::remove_cvref_t<T>::stream_width;

// Recursive deduction: pick the first stream arg's width.
template <typename...> struct deduce_width;
template <> struct deduce_width<> { static constexpr size_t value = 0; };

template <typename First, typename... Rest>
struct deduce_width<First, Rest...> {
    static constexpr size_t value = is_any_stream_v<First>
        ? stream_width_of_v<First>
        : deduce_width<Rest...>::value;
};

template <typename... Args>
inline constexpr size_t deduce_width_v =
    deduce_width<std::remove_cvref_t<Args>...>::value;

// --- Tag --------------------------------------------------------------------

// Per-arg helper: evaluates to T's tag_t when T is a stream, else void.
template <typename T> struct tag_of_impl { using type = void; };
template <typename T>
requires (is_any_stream_v<T>)
struct tag_of_impl<T> {
    using type = typename std::remove_cvref_t<T>::tag_t;
};
template <typename T> using tag_of_t = typename tag_of_impl<T>::type;

// Recursive deduction via constrained partial specializations: pick the
// first stream arg's tag_t.
template <typename...> struct deduce_tag;
template <> struct deduce_tag<> { using type = void; };

template <typename First, typename... Rest>
requires (is_any_stream_v<First>)
struct deduce_tag<First, Rest...> {
    using type = typename std::remove_cvref_t<First>::tag_t;
};

template <typename First, typename... Rest>
requires (!is_any_stream_v<First>)
struct deduce_tag<First, Rest...> : deduce_tag<Rest...> {};

// --- Stored type ------------------------------------------------------------

template <typename T> struct stored_of_impl { using type = void; };
template <typename T>
requires (is_vec_stream_v<T>)
struct stored_of_impl<T> {
    using type = typename std::remove_cvref_t<T>::stored_type;
};
template <typename T> using stored_of_t = typename stored_of_impl<T>::type;

template <typename...> struct deduce_stored;
template <> struct deduce_stored<> { using type = void; };

template <typename First, typename... Rest>
requires (is_vec_stream_v<First>)
struct deduce_stored<First, Rest...> {
    using type = typename std::remove_cvref_t<First>::stored_type;
};

template <typename First, typename... Rest>
requires (!is_vec_stream_v<First>)
struct deduce_stored<First, Rest...> : deduce_stored<Rest...> {};

// --- Consistency checks (use the safe per-arg helpers, NOT direct member
//     access) -----------------------------------------------------------------

template <typename... Args>
inline constexpr bool widths_consistent_v =
    ((!is_any_stream_v<Args> ||
      stream_width_of_v<Args> == deduce_width_v<Args...>)
     && ... && true);

template <typename... Args>
inline constexpr bool tags_consistent_v =
    ((!is_any_stream_v<Args> ||
      std::is_same_v<tag_of_t<Args>,
                     typename deduce_tag<std::remove_cvref_t<Args>...>::type>)
     && ... && true);

/// Helper: evaluate `op` at slot S, with per-arg projection. Expanding the
/// `args` pack INSIDE this helper (at depth 0) leaves only `S` to be
/// expanded by the caller at the outer pack-expansion site. C++ pack
/// expansion expands all packs mentioned at the same depth simultaneously
/// (zip-style); we want cartesian, so we hide one pack behind a template
/// barrier.
///
/// Args are taken by FORWARDING reference and `project<S>` is invoked with
/// the actual reference category preserved. The `project` overload returns
/// `arg[S]` for stream args and `(arg)` (lvalue ref) for non-stream args
/// — same semantics as before, just consistent with the forwarding-ref
/// pattern of the outer `lift_op`.
template <size_t S, typename Op, typename... Args>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto lift_eval_at_slot(Op& op, Args&&... args) {
    return op(project<S>(args)...);
}

template <size_t S, typename Op, typename... Args>
[[gnu::always_inline]] HWY_INLINE
void lift_apply_at_slot(Op& op, Args&&... args) {
    op(project<S>(args)...);
}

/// lift_op_pack_vec: helper that does the actual pack expansion for a
/// vec_stream-returning lift. The result is constructed via `pack(...)`
/// — a single std::array aggregate-init — so the compiler sees one point
/// of construction (NRVO-friendly).
template <class D, typename ST, size_t W,
          size_t... S, typename Op, typename... Args>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vec_stream<D, ST, W> lift_op_pack_vec(
    std::index_sequence<S...>, Op& op, Args&&... args) {
    return vec_stream<D, ST, W>::pack(
        lift_eval_at_slot<S>(op, std::forward<Args>(args)...)...);
}

template <class D, size_t W,
          size_t... S, typename Op, typename... Args>
[[nodiscard, gnu::always_inline]] HWY_INLINE
mask_stream<D, W> lift_op_pack_mask(
    std::index_sequence<S...>, Op& op, Args&&... args) {
    return mask_stream<D, W>::pack(
        lift_eval_at_slot<S>(op, std::forward<Args>(args)...)...);
}

template <size_t... S, typename Op, typename... Args>
[[gnu::always_inline]] HWY_INLINE
void lift_op_apply_void(
    std::index_sequence<S...>, Op& op, Args&&... args) {
    (lift_apply_at_slot<S>(op, std::forward<Args>(args)...), ...);
}

/// lift_op: invoke `op(project<S>(args)...)` for S in [0, W) and bundle the
/// W results into the matching stream type.
///
/// Bundle type is decided by the RESULT-element type, not by argument types:
///   element result is hn::Vec<D> → vec_stream<D, ST, W>
///   element result is hn::Mask<D> → mask_stream<D, W>
///   element result is void        → void (used by side-effect ops like StoreStream)
///
/// Implementation note: each non-void path calls one of the static
/// `lift_op_pack_*` helpers above which take a `std::index_sequence<S...>`
/// and a parameter pack of args by reference, then build the result via
/// `vec_stream::pack(op(project<S>(args)...)...)`. The pack form is a
/// single direct construction (no "default-construct + per-slot assign"
/// cycle); the optimizer sees a single std::array aggregate init and
/// scalar-replaces the bundle's storage in the calling frame.
template <typename Op, typename... Args>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto lift_op(Op&& op, Args&&... args) {
    static_assert(any_arg_is_stream<Args...>,
                  "lift_op requires at least one stream argument");
    static_assert(widths_consistent_v<Args...>,
                  "lift_op: all stream arguments must share W");
    static_assert(tags_consistent_v<Args...>,
                  "lift_op: all stream arguments must share tag_t");

    constexpr size_t W = deduce_width_v<Args...>;
    using D = typename deduce_tag<std::remove_cvref_t<Args>...>::type;
    static_assert(!std::is_void_v<D>, "lift_op: tag deduction failed");

    using elem_result_t =
        std::remove_cvref_t<decltype(op(project<0>(args)...))>;

    if constexpr (std::is_same_v<elem_result_t, hn::Vec<D>>) {
        // Stored type: take the first vec_stream argument's, fall back to
        // hn::TFromD<D> if no vec_stream arg exists (e.g. IfThenElse with
        // a mask_stream + plain vecs producing a Vec<D>).
        using ST_from_args = typename deduce_stored<std::remove_cvref_t<Args>...>::type;
        using ST = std::conditional_t<std::is_void_v<ST_from_args>,
                                       hn::TFromD<D>, ST_from_args>;
        return lift_op_pack_vec<D, ST, W>(
            std::make_index_sequence<W>{}, op, std::forward<Args>(args)...);
    } else if constexpr (std::is_same_v<elem_result_t, hn::Mask<D>>) {
        return lift_op_pack_mask<D, W>(
            std::make_index_sequence<W>{}, op, std::forward<Args>(args)...);
    } else {
        static_assert(std::is_same_v<elem_result_t, void>,
                      "lift_op: element result must be Vec<D> or Mask<D> or void");
        lift_op_apply_void(std::make_index_sequence<W>{}, op,
            std::forward<Args>(args)...);
    }
}

// ============================================================================
// Operation tag structs — one per Highway op.
// Each is two lines; not constexpr because hn::* ops are not constexpr.
// ============================================================================

#define BIOSEQ_VS_DEFINE_TAG(NAME)                                                   \
    struct op_##NAME {                                                               \
        template <typename... Xs> [[gnu::always_inline]] HWY_INLINE                  \
        static auto apply(Xs&&... xs) {                                              \
            return hn::NAME(std::forward<Xs>(xs)...);                                \
        }                                                                            \
    }
// NOTE — this macro is purely a NAMING shortcut: it expands to a struct with
// a static apply() forwarding to hn::<Name>. There is no logic in the macro,
// it is exactly the long-hand form repeated 25 times. main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md forbids
// macros for *code logic*; this is type-name boilerplate that has no other
// expression. We #undef it immediately after the registrations to limit blast
// radius. The §10.B fallback in the plan was the long-hand form (~280 LOC of
// pure repetition); we ship the macro form for compactness and use comment
// blocks to delimit it explicitly.

BIOSEQ_VS_DEFINE_TAG(Add);
BIOSEQ_VS_DEFINE_TAG(Sub);
BIOSEQ_VS_DEFINE_TAG(Mul);
BIOSEQ_VS_DEFINE_TAG(Min);
BIOSEQ_VS_DEFINE_TAG(Max);
BIOSEQ_VS_DEFINE_TAG(And);
BIOSEQ_VS_DEFINE_TAG(Or);
BIOSEQ_VS_DEFINE_TAG(Xor);
BIOSEQ_VS_DEFINE_TAG(AndNot);
BIOSEQ_VS_DEFINE_TAG(Not);
BIOSEQ_VS_DEFINE_TAG(Neg);
BIOSEQ_VS_DEFINE_TAG(Abs);
BIOSEQ_VS_DEFINE_TAG(Eq);
BIOSEQ_VS_DEFINE_TAG(Ne);
BIOSEQ_VS_DEFINE_TAG(Lt);
BIOSEQ_VS_DEFINE_TAG(Gt);
BIOSEQ_VS_DEFINE_TAG(Le);
BIOSEQ_VS_DEFINE_TAG(Ge);
BIOSEQ_VS_DEFINE_TAG(ShiftLeftSame);
BIOSEQ_VS_DEFINE_TAG(ShiftRightSame);
BIOSEQ_VS_DEFINE_TAG(IfThenElse);
BIOSEQ_VS_DEFINE_TAG(IfThenElseZero);
BIOSEQ_VS_DEFINE_TAG(IfThenZeroElse);
BIOSEQ_VS_DEFINE_TAG(MaskFromVec);

#undef BIOSEQ_VS_DEFINE_TAG

}  // namespace vec_stream_internal

/// Single-template wrapper that any op can be registered with. The constraint
/// is intentional defense-in-depth alongside lift_op's static_assert: at the
/// wrapper layer it produces a clean SFINAE failure for plain-vec calls; at
/// lift_op's layer the static_assert produces a more descriptive error if a
/// future direct caller bypasses `lifted`.
template <typename OpTag, typename... Args>
requires (any_arg_is_stream<Args...>)
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto lifted(Args&&... args) {
    return vec_stream_internal::lift_op(
        []<typename... Xs>(Xs&&... xs) [[gnu::always_inline]] {
            return OpTag::apply(std::forward<Xs>(xs)...);
        },
        std::forward<Args>(args)...);
}

// ============================================================================
// User-facing free-function wrappers — one per op.
// All are `auto`, NOT constexpr (their bodies eventually call non-constexpr
// hn::* ops). Each is a single-line forward to lifted<OpTag>.
// ============================================================================

#define BIOSEQ_VS_BINARY_WRAPPER(NAME)                                              \
    template <typename A, typename B>                                                \
    requires (any_arg_is_stream<A, B>)                                               \
    [[nodiscard]] HWY_INLINE auto NAME(A&& a, B&& b) {                               \
        return lifted<vec_stream_internal::op_##NAME>(                               \
            std::forward<A>(a), std::forward<B>(b));                                 \
    }

#define BIOSEQ_VS_UNARY_WRAPPER(NAME)                                               \
    template <typename A>                                                            \
    requires (is_any_stream_v<A>)                                                    \
    [[nodiscard]] HWY_INLINE auto NAME(A&& a) {                                      \
        return lifted<vec_stream_internal::op_##NAME>(std::forward<A>(a));           \
    }

#define BIOSEQ_VS_TERNARY_WRAPPER(NAME)                                             \
    template <typename A, typename B, typename C>                                    \
    requires (any_arg_is_stream<A, B, C>)                                            \
    [[nodiscard]] HWY_INLINE auto NAME(A&& a, B&& b, C&& c) {                        \
        return lifted<vec_stream_internal::op_##NAME>(                               \
            std::forward<A>(a), std::forward<B>(b), std::forward<C>(c));             \
    }

// Binary lifts — both vec/vec and mask/mask are handled here; the constraint
// `any_arg_is_stream` matches whichever stream type the user supplies.
BIOSEQ_VS_BINARY_WRAPPER(Add)
BIOSEQ_VS_BINARY_WRAPPER(Sub)
BIOSEQ_VS_BINARY_WRAPPER(Mul)
BIOSEQ_VS_BINARY_WRAPPER(Min)
BIOSEQ_VS_BINARY_WRAPPER(Max)
BIOSEQ_VS_BINARY_WRAPPER(And)
BIOSEQ_VS_BINARY_WRAPPER(Or)
BIOSEQ_VS_BINARY_WRAPPER(Xor)
BIOSEQ_VS_BINARY_WRAPPER(AndNot)
BIOSEQ_VS_BINARY_WRAPPER(Eq)
BIOSEQ_VS_BINARY_WRAPPER(Ne)
BIOSEQ_VS_BINARY_WRAPPER(Lt)
BIOSEQ_VS_BINARY_WRAPPER(Gt)
BIOSEQ_VS_BINARY_WRAPPER(Le)
BIOSEQ_VS_BINARY_WRAPPER(Ge)
BIOSEQ_VS_BINARY_WRAPPER(ShiftLeftSame)
BIOSEQ_VS_BINARY_WRAPPER(ShiftRightSame)

// Unary lifts.
BIOSEQ_VS_UNARY_WRAPPER(Not)
BIOSEQ_VS_UNARY_WRAPPER(Neg)
BIOSEQ_VS_UNARY_WRAPPER(Abs)
BIOSEQ_VS_UNARY_WRAPPER(MaskFromVec)

// Ternary lifts (full {stream, scalar}^3 = 27 cases handled in one wrapper).
BIOSEQ_VS_TERNARY_WRAPPER(IfThenElse)
BIOSEQ_VS_TERNARY_WRAPPER(IfThenElseZero)
BIOSEQ_VS_TERNARY_WRAPPER(IfThenZeroElse)

#undef BIOSEQ_VS_BINARY_WRAPPER
#undef BIOSEQ_VS_UNARY_WRAPPER
#undef BIOSEQ_VS_TERNARY_WRAPPER

// ============================================================================
// Templated shifts — non-type template parameter, written long-hand.
// ============================================================================

template <int N, typename A>
requires (is_vec_stream_v<A>)
[[nodiscard]] HWY_INLINE auto ShiftLeft(A&& a) {
    return vec_stream_internal::lift_op(
        [](auto&& x) [[gnu::always_inline]] { return hn::ShiftLeft<N>(x); },
        std::forward<A>(a));
}

template <int N, typename A>
requires (is_vec_stream_v<A>)
[[nodiscard]] HWY_INLINE auto ShiftRight(A&& a) {
    return vec_stream_internal::lift_op(
        [](auto&& x) [[gnu::always_inline]] { return hn::ShiftRight<N>(x); },
        std::forward<A>(a));
}

template <int N, typename A>
requires (is_vec_stream_v<A>)
[[nodiscard]] HWY_INLINE auto RotateLeft(A&& a) {
    return vec_stream_internal::lift_op(
        [](auto&& x) [[gnu::always_inline]] { return hn::RotateLeft<N>(x); },
        std::forward<A>(a));
}

template <int N, typename A>
requires (is_vec_stream_v<A>)
[[nodiscard]] HWY_INLINE auto RotateRight(A&& a) {
    return vec_stream_internal::lift_op(
        [](auto&& x) [[gnu::always_inline]] { return hn::RotateRight<N>(x); },
        std::forward<A>(a));
}

// ============================================================================
// Bespoke wrappers — ops whose tag/result-type cannot be inferred via lift_op.
// ============================================================================

namespace vec_stream_internal_bc {
template <class D_to, class A, size_t S>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto bitcast_at_slot(D_to d_to, A const& a) noexcept {
    return hn::BitCast(d_to, a[S]);
}

template <class D_to, typename ST_to, size_t W,
          class A, size_t... S>
[[nodiscard, gnu::always_inline]] HWY_INLINE
vec_stream<D_to, ST_to, W> bitcast_pack_impl(
    D_to d_to, A const& a, std::index_sequence<S...>) noexcept {
    return vec_stream<D_to, ST_to, W>::pack(
        bitcast_at_slot<D_to, A, S>(d_to, a)...);
}
}  // namespace vec_stream_internal_bc

/// BitCast: takes a destination tag D_to and a vec_stream, returning a stream
/// of the destination type. Cannot route through lift_op because the result
/// tag is the user-supplied D_to, not the input stream's tag.
template <class D_to, typename A>
requires (is_vec_stream_v<A>)
[[nodiscard]] HWY_INLINE auto BitCast(D_to d_to, A&& a) {
    using ATy = std::remove_cvref_t<A>;
    constexpr size_t W = ATy::stream_width;
    using ST_to = hn::TFromD<D_to>;
    return vec_stream_internal_bc::bitcast_pack_impl<D_to, ST_to, W>(
        d_to, a, std::make_index_sequence<W>{});
}

/// VecFromMask: mask_stream → vec_stream. Tag is supplied explicitly.
template <class D, typename A>
requires (is_mask_stream_v<A>)
[[nodiscard]] HWY_INLINE auto VecFromMask(D d, A&& a) {
    using ATy = std::remove_cvref_t<A>;
    static_assert(std::is_same_v<typename ATy::tag_t, D>, "VecFromMask: tag mismatch");
    return vec_stream_internal::lift_op(
        [d](auto&& m) [[gnu::always_inline]] { return hn::VecFromMask(d, m); },
        std::forward<A>(a));
}

/// StoreStream: per-stream store to W separate base pointers. The pointers
/// must be supplied as a `std::array<T*, W>` (the size cannot be deduced from
/// brace-init). Renamed from `Store` to avoid collisions with hn::Store under
/// `using namespace hn;`.
template <class D, typename A, typename T>
requires (is_vec_stream_v<A>)
HWY_INLINE void StoreStream(A const& stream, D d,
                            std::array<T*, std::remove_cvref_t<A>::stream_width> ptrs) {
    using ATy = std::remove_cvref_t<A>;
    static_assert(std::is_same_v<typename ATy::tag_t, D>, "StoreStream: tag mismatch");
    for_each_index<ATy::stream_width>([&](auto s_const) [[gnu::always_inline]] {
        constexpr size_t s = decltype(s_const)::value;
        hn::Store(stream[s], d, ptrs[s]);
    });
}

template <class D, typename A, typename T>
requires (is_vec_stream_v<A>)
HWY_INLINE void StoreUStream(A const& stream, D d,
                             std::array<T*, std::remove_cvref_t<A>::stream_width> ptrs) {
    using ATy = std::remove_cvref_t<A>;
    static_assert(std::is_same_v<typename ATy::tag_t, D>, "StoreUStream: tag mismatch");
    for_each_index<ATy::stream_width>([&](auto s_const) [[gnu::always_inline]] {
        constexpr size_t s = decltype(s_const)::value;
        hn::StoreU(stream[s], d, ptrs[s]);
    });
}

/// AllFalsePerStream: collapses each stream's mask to a bool, returning W bools.
/// The cross-stream OR is left to the consumer (typically `||` or `std::ranges::any_of`).
template <class D, typename A>
requires (is_mask_stream_v<A>)
[[nodiscard]] HWY_INLINE auto AllFalsePerStream(D d, A const& a)
    -> std::array<bool, std::remove_cvref_t<A>::stream_width>
{
    using ATy = std::remove_cvref_t<A>;
    static_assert(std::is_same_v<typename ATy::tag_t, D>, "AllFalsePerStream: tag mismatch");
    std::array<bool, ATy::stream_width> out{};
    for_each_index<ATy::stream_width>([&](auto s_const) [[gnu::always_inline]] {
        constexpr size_t s = decltype(s_const)::value;
        out[s] = hn::AllFalse(d, a[s]);
    });
    return out;
}

/// Per-stream compile-time loop over the W streams of a stream-shaped layout.
/// Use this when you need to do something *per stream* that doesn't fit the
/// `lifted` machinery (e.g. invoke a per-stream non-`vec_stream` matrix
/// builder, run scalar bookkeeping per stream during emit). Just an alias for
/// `for_each_index<W>` — the rename signals that the loop body is iterating
/// over stream slots so callers don't have to reach for `bioseq_simd.utils`
/// just for the index loop.
template <size_t W, typename F>
[[gnu::always_inline]] HWY_INLINE
constexpr void for_each_stream(F&& fn) {
    for_each_index<W>(std::forward<F>(fn));
}

/// Build a `vec_stream<D, ST, W>` whose slot S holds `factory(ic<S>{})`. The
/// factory is invoked W times with `std::integral_constant<size_t, S>` so each
/// slot can resolve at compile time. Centralises the index_sequence + pack
/// construction pattern so callers don't repeat the IIFE — see also
/// `make_vec_stream_from_per_stream` for the common matrix-row case.
template <class D, typename ST, size_t W, typename Factory>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto make_vec_stream(Factory&& factory) noexcept {
    return [&]<size_t... S>(std::index_sequence<S...>) [[gnu::always_inline]] {
        return vec_stream<D, ST, W>::pack(
            factory(std::integral_constant<size_t, S>{})...);
    }(std::make_index_sequence<W>{});
}

/// Pack one row out of a `std::array<std::array<vec_t, MaxLanes>, W>` matrix
/// bundle into a `vec_stream`. The outer array indexes the stream, the inner
/// array indexes the row within that stream's matrix. Inlined to a single
/// `pack(...)` expression — no runtime loop.
template <class D, typename ST, size_t W, typename PerStreamMatrix>
[[nodiscard, gnu::always_inline]] HWY_INLINE
auto make_vec_stream_from_per_stream(
    PerStreamMatrix const& per_stream_matrix, size_t row_index) noexcept {
    return make_vec_stream<D, ST, W>([&](auto s_const) [[gnu::always_inline]] {
        return per_stream_matrix[decltype(s_const)::value][row_index];
    });
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export
