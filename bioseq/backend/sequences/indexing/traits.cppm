/**
 * @file traits.cppm
 * @brief Checks the metadata and build methods of registered indexes.
 *
 * index_descriptor gives the registry an index's layout before it chooses
 * a storage type and instantiates the index template. NO_POSTGRES.
 */
module;
#include <concepts>
#include <cstdint>
#include <string_view>
export module bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/**
 * @brief Describes an index before a storage type is chosen.
 *
 * Each registered index specializes this template with index_type, index_name
 * and allows_update_on_insert.
 */
template <template <class> class p_index_template> struct index_descriptor;

/** @brief Requires the expected layout, a compatible storage type and an index name. */
template <class p_index, sequence_index_layout p_layout>
concept is_index_for =
    (p_index::index_type == p_layout) &&
    is_index_chunked_storage_for<typename p_index::storage_type, p_layout> && requires {
        { std::string_view{p_index::index_name} }
            -> std::same_as<std::string_view>;
    };

/** @brief Requires an index with one of the supported storage layouts. */
template <class p_index>
concept index_for_any = is_index_for<p_index, sequence_index_layout::fragment_chunks> ||
                        is_index_for<p_index, sequence_index_layout::sequence_inline> ||
                        is_index_for<p_index, sequence_index_layout::table_chunks>;

/** @brief Requires build and rebuild methods accepting storage and a sequence. */
template <class p_index, class p_alphabet>
concept index_provides_build_fragmented =
    sequence_alphabet<p_alphabet> && requires(typename p_index::storage_type& storage,
                                              bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        { p_index::build(storage, seq) } -> std::same_as<void>;
        { p_index::rebuild(storage, seq) } -> std::same_as<void>;
    };

/** @brief Requires build and rebuild methods accepting a selected minimizer backend. */
template <class p_index, class p_alphabet>
concept index_provides_backend_build_fragmented =
    sequence_alphabet<p_alphabet> && requires(typename p_index::storage_type& storage,
                                              bioseq::storage::lazy_sequence_view<p_alphabet> seq,
                                              bioseq::minimizers::minimizer_sketch_backend backend,
                                              const bioseq::minimizers::minimizer_kernels* kernels,
                                              std::uint8_t alphabet_code) {
        { p_index::build(storage, seq, backend, kernels, alphabet_code) } -> std::same_as<void>;
        { p_index::rebuild(storage, seq, backend, kernels, alphabet_code) } -> std::same_as<void>;
    };

/** @brief Requires inline index build and rebuild methods for the given alphabet. */
template <class p_index, class p_alphabet>
concept index_provides_build_inline =
    sequence_alphabet<p_alphabet> && index_provides_build_fragmented<p_index, p_alphabet>;

/** @brief Requires table index build and rebuild methods accepting storage alone. */
template <class p_index>
concept index_provides_build_table = requires(typename p_index::storage_type& storage) {
    { p_index::build(storage) } -> std::same_as<void>;
    { p_index::rebuild(storage) } -> std::same_as<void>;
};
} // namespace bioseq::indexing
