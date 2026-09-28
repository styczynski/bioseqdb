/**
 * @file standard_library_link_support.cppm
 * @brief Supplies standard-library definitions missing  from GCC module objects.
 */
/*
 * GCC 16.1 can leave calls to inline standard-library members unresolved in
 * objects that import std. PostgreSQL then cannot load the extension.
 * This include-based translation unit takes their addresses so GCC emits
 * the missing definitions, including when compiling with otimization
 *
 * The affected members are span<unsigned char>::data(), span<unsigned char>::size(),
 * and pair<size_t, size_t> move assignment. Keep the types and C++ ABI settings
 * (-fabi-version=0) the same as the module objects so their symbol names match.
 */
#include <cstddef>
#include <span>
#include <utility>

namespace {

using shim_span = std::span<unsigned char>;
using shim_pair = std::pair<std::size_t, std::size_t>;

/*
 * Keep these pointers in the object even though no code reads them. Their
 * initializers force GCC to emit linkable definitions of the member functions.
 */
/** @brief Takes the address of span::data to emit its missing definition. */
[[gnu::used]] auto const bioseq_external_keep_span_data = &shim_span::data;
/** @brief Takes the address of span::size to emit its missing definition. */
[[gnu::used]] auto const bioseq_external_keep_span_size = &shim_span::size;
/** @brief Takes the  address of pair move assignment to emit its definition. */
[[gnu::used]] shim_pair& (shim_pair::* const bioseq_external_keep_pair_move)(
    shim_pair&&) = &shim_pair::operator=;

}  // namespace
