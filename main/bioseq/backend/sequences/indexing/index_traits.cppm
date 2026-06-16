/**
 * @file index_traits.cppm
 * @brief DOC0279 §7.2 — `index_descriptor`, `is_index_for`, `index_provides_*`.
 *
 * The storage-free `index_descriptor` breaks the cycle "need index_type to pick
 * storage, need storage to read index_type". NO_POSTGRES.
 */
module;
#include <concepts>
#include <string_view>
export module bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.storage.lazy;     // lazy_compact_sequence<R,Scanner>
import bioseq.storage.size;     // sequence_size_t
import bioseq.storage.scanner;  // is_valid_sequence_scanner<Scanner>
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
template <class T, class R, class Scanner>
concept index_provides_build_fragmented =
    bioseq::storage::is_valid_sequence_scanner<Scanner> &&
    requires (
    typename T::storage_type& s, bioseq::storage::lazy_compact_sequence<R, Scanner>& seq) {
    { T::build(s, seq) }   -> std::same_as<void>;
    { T::rebuild(s, seq) } -> std::same_as<void>;
};
template <class T, class R, class Scanner>
concept index_provides_build_inline = index_provides_build_fragmented<T, R, Scanner>;  // same shape
template <class T>
concept index_provides_build_table = requires (typename T::storage_type& s) {
    { T::build(s) }   -> std::same_as<void>;
    { T::rebuild(s) } -> std::same_as<void>;
};
}  // namespace bioseq::indexing
