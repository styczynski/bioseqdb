/**
 * @file standard_library_link_support.cppm
 * @brief Forces emission of standard-library members required by GCC module objects.
 */
/*-------------------------------------------------------------------------
 *
 * external/standard_library_link_support.cppm
 *
 * GCC 16.1's `import std` code generation leaves a
 * handful of trivial inline std member functions *referenced but not emitted*
 * in module objects that use them. A module unit emits an out-of-line call
 * and expects the definition to come from elsewhere, but no object provides it.
 * The extension's unresolved set is exactly:
 *
 *   std::span<unsigned char>::data() const
 *   std::span<unsigned char>::size() const
 *   std::pair<std::size_t,std::size_t>::operator=(pair&&)   (the constrained one)
 *
 * (referenced by e.g. backend/pg/table/access_method.cppm). dlopen() of the
 * extension then fails with "undefined symbol". This remains an include-based
 * translation unit so GCC emits normal COMDAT weak definitions
 * here — and it force-emits those definitions by taking the address of each
 * member (address-of forces the out-of-line body regardless of optimization
 * level). The manglings match the module references because the types and ABI
 * (-fabi-version=0) are identical. No behavior, no dependency on the provider
 * stack; safe to compile unconditionally.
 *
 *-------------------------------------------------------------------------
 */
#include <cstddef>
#include <span>
#include <utility>

namespace {

using shim_span = std::span<unsigned char>;
using shim_pair = std::pair<std::size_t, std::size_t>;

/*
 * [[gnu::used]] keeps these internal-linkage pointers (and thus the odr-uses in
 * their initializers) in the object even though nothing references them; the
 * odr-use of each member function emits its external COMDAT weak definition,
 * which is what the module objects link against.
 */
/** @brief ODR-uses `span::data` so the linker receives its weak definition. */
[[gnu::used]] auto const bioseq_external_keep_span_data = &shim_span::data;
/** @brief ODR-uses `span::size` so the linker receives its weak definition. */
[[gnu::used]] auto const bioseq_external_keep_span_size = &shim_span::size;
/** @brief ODR-uses pair move assignment so the linker receives its definition. */
[[gnu::used]] shim_pair& (shim_pair::* const bioseq_external_keep_pair_move)(
    shim_pair&&) = &shim_pair::operator=;

}  // namespace
