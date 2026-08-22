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
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences.kmers.highway_minimizer_contract;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/**
 * @brief Provides storage-free metadata for an index template.
 *
 * Every registered index specializes this descriptor with `index_type`,
 * `index_name`, and `allows_update_on_insert` members.
 *
 * @tparam p_index_template Index template described by the specialization
 */
template <template <class> class p_index_template> struct index_descriptor;

/**
 * @brief Checks that a bound index conforms to one persisted layout.
 * @tparam p_index Fully instantiated index type
 * @tparam p_layout Expected persisted layout
 */
template <class p_index, sequence_index_layout p_layout>
concept is_index_for =
    (p_index::index_type == p_layout) &&
    is_index_chunked_storage_for<typename p_index::storage_type, p_layout> && requires {
        { std::string_view{p_index::index_name} }
            -> std::same_as<std::string_view>;
    };

/**
 * @brief Checks that a bound index conforms to any supported layout.
 * @tparam p_index Fully instantiated index type
 */
template <class p_index>
concept index_for_any = is_index_for<p_index, sequence_index_layout::fragment_chunks> ||
                        is_index_for<p_index, sequence_index_layout::sequence_inline> ||
                        is_index_for<p_index, sequence_index_layout::table_chunks>;

/**
 * @brief Checks fragmented build and rebuild entry points for an alphabet.
 * @tparam p_index Fully instantiated index type
 * @tparam p_alphabet Sequence alphabet accepted by the index
 */
template <class p_index, class p_alphabet>
concept index_provides_build_fragmented =
    sequence_alphabet<p_alphabet> && requires(typename p_index::storage_type& storage,
                                              bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        { p_index::build(storage, seq) } -> std::same_as<void>;
        { p_index::rebuild(storage, seq) } -> std::same_as<void>;
    };

/**
 * @brief Checks fragmented build entry points with a selected minimizer backend.
 * @tparam p_index Fully instantiated index type
 * @tparam p_alphabet Sequence alphabet accepted by the index
 */
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

/**
 * @brief Checks inline build and rebuild entry points for an alphabet.
 * @tparam p_index Fully instantiated index type
 * @tparam p_alphabet Sequence alphabet accepted by the index
 */
template <class p_index, class p_alphabet>
concept index_provides_build_inline =
    sequence_alphabet<p_alphabet> && index_provides_build_fragmented<p_index, p_alphabet>;

/**
 * @brief Checks table-wide build and rebuild entry points.
 * @tparam p_index Fully instantiated index type
 */
template <class p_index>
concept index_provides_build_table = requires(typename p_index::storage_type& storage) {
    { p_index::build(storage) } -> std::same_as<void>;
    { p_index::rebuild(storage) } -> std::same_as<void>;
};
} // namespace bioseq::indexing
