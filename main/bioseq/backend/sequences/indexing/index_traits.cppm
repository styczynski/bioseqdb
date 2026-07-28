/**
 * @file index_traits.cppm
 * @brief DOC0279 §7.2 — `index_descriptor`, `is_index_for`, `index_provides_*`.
 *
 * The storage-free `index_descriptor` breaks the cycle "need index_type to pick
 * storage, need storage to read index_type". NO_POSTGRES.
 */
module;
#include <hwy/highway.h>  // DOC0345 Patch H — HWY_NAMESPACE macro names MinimizerSketchBackend
#include <concepts>
#include <string_view>
export module bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.sequences;
import bioseq.storage.lazy;     // canonical basic_lazy_sequence_view
import bioseq.storage.size;     // sequence_size_t
import bioseq.sequences.kmers;  // DOC0345 Patch H — MinimizerSketchBackend
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Storage-free metadata for an index TEMPLATE. Source of truth for name+type;
/// every index specialises this in its own module. REQUIRED members of every
/// specialization: `static constexpr SequenceIndexType index_type;`,
/// `static constexpr std::string_view index_name;`,
/// `static constexpr bool allows_update_on_insert;`.
template <template <class> class IndexTmpl>
struct index_descriptor;   // primary undefined -> using an unregistered index is a hard error

/// Bound-index conformance: a fully-instantiated index I (with storage_type) is
/// a valid index for layout M.
template <class T, SequenceIndexType M>
concept is_index_for =
       (T::index_type == M)
    && is_index_chunked_storage_for<typename T::storage_type, M>
    && requires { { std::string_view{T::index_name} } -> std::same_as<std::string_view>; };

template <class T>
concept index_for_any =
       is_index_for<T, SequenceIndexType::CHUNKED_PER_FRAGMENT>
    || is_index_for<T, SequenceIndexType::INLINE_PER_SEQUENCE>
    || is_index_for<T, SequenceIndexType::CHUNKED_PER_TABLE>;

/// Parametrised build/rebuild checks (used by supported_indexes' methods, and by
/// the pg-side static_asserts which CAN name a concrete range/scanner).
template <class T, class Alphabet>
concept index_provides_build_fragmented =
    sequence_alphabet<Alphabet> && requires (
    typename T::storage_type& s,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> seq) {
    { T::build(s, seq) }   -> std::same_as<void>;
    { T::rebuild(s, seq) } -> std::same_as<void>;
};
// DOC0345 Patch H — stronger optional check: the index exposes the explicit
// 3-arg backend build/rebuild. The registry uses the scalar (2-arg) overload for
// conformance and the explicit backend overload for PG-selected builds.
template <class T, class Alphabet>
concept index_provides_backend_build_fragmented =
    sequence_alphabet<Alphabet> && requires (
        typename T::storage_type& storage,
        bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
        bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend backend) {
        { T::build(storage, seq, backend) } -> std::same_as<void>;
        { T::rebuild(storage, seq, backend) } -> std::same_as<void>;
    };
template <class T, class Alphabet>
concept index_provides_build_inline =
    sequence_alphabet<Alphabet>
    && index_provides_build_fragmented<T, Alphabet>;  // same shape
template <class T>
concept index_provides_build_table = requires (typename T::storage_type& s) {
    { T::build(s) }   -> std::same_as<void>;
    { T::rebuild(s) } -> std::same_as<void>;
};
}  // namespace bioseq::indexing
