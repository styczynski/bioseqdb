/**
 * @file traits.cppm
 * @brief Compile-time index descriptors and capability contracts.
 *
 * The storage-free `index_descriptor` breaks the cycle "need index_type to pick
 * storage, need storage to read index_type". NO_POSTGRES.
 */
module;
#include <concepts>
#include <cstdint>
#include <string_view>
export module bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;     // canonical lazy_sequence_view
import bioseq.storage.size;     // sequence_size_t
import bioseq.sequences.kmers.highway_minimizer_contract;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Storage-free metadata for an index TEMPLATE. Source of truth for name+type;
/// every index specialises this in its own module. REQUIRED members of every
/// specialization: `static constexpr sequence_index_layout index_type;`,
/// `static constexpr std::string_view index_name;`,
/// `static constexpr bool allows_update_on_insert;`.
template <template <class> class p_index_template>
struct index_descriptor;   // primary undefined -> using an unregistered index is a hard error

/// Bound-index conformance: a fully-instantiated index I (with storage_type) is
/// a valid index for layout M.
template <class p_index, sequence_index_layout p_layout>
concept is_index_for =
       (p_index::index_type == p_layout)
    && is_index_chunked_storage_for<
           typename p_index::storage_type, p_layout>
    && requires {
        { std::string_view{p_index::index_name} }
            -> std::same_as<std::string_view>;
    };

template <class p_index>
concept index_for_any =
       is_index_for<p_index, sequence_index_layout::fragment_chunks>
    || is_index_for<p_index, sequence_index_layout::sequence_inline>
    || is_index_for<p_index, sequence_index_layout::table_chunks>;

/// Parametrised build/rebuild checks (used by supported_indexes' methods, and by
/// the pg-side static_asserts which CAN name a concrete range/scanner).
template <class p_index, class p_alphabet>
concept index_provides_build_fragmented =
    sequence_alphabet<p_alphabet> && requires (
    typename p_index::storage_type& s,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
    { p_index::build(s, seq) }   -> std::same_as<void>;
    { p_index::rebuild(s, seq) } -> std::same_as<void>;
};
// The explicit backend surface also carries the already-selected immutable
// minimizer table. SIMD index generation must never redispatch in a leaf.
template <class p_index, class p_alphabet>
concept index_provides_backend_build_fragmented =
    sequence_alphabet<p_alphabet> && requires (
        typename p_index::storage_type& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        { p_index::build(storage, seq, backend, kernels, alphabet_code) }
            -> std::same_as<void>;
        { p_index::rebuild(storage, seq, backend, kernels, alphabet_code) }
            -> std::same_as<void>;
    };
template <class p_index, class p_alphabet>
concept index_provides_build_inline =
    sequence_alphabet<p_alphabet>
    && index_provides_build_fragmented<
           p_index, p_alphabet>;  // same shape
template <class p_index>
concept index_provides_build_table =
    requires (typename p_index::storage_type& s) {
    { p_index::build(s) }   -> std::same_as<void>;
    { p_index::rebuild(s) } -> std::same_as<void>;
};
}  // namespace bioseq::indexing
