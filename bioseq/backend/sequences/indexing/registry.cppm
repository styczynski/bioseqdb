/**
 * @file registry.cppm
 * @brief Compile-time registry for supported sequence indexes.
 *
 * The registry is the substrate the indexes build on. It does not import the
 * index modules. Every TU that instantiates `supported_indexes<...>` must import
 * the index modules whose descriptors it reads (the §7.3 static_assert turns a
 * forgotten import into a diagnosed error). NO_POSTGRES.
 */
module;
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.indexing.registry;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers.highway_minimizer_contract;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/**
 * @brief Binds registered index templates to their storage implementations.
 * @tparam p_fragment_storage Per-fragment chunk storage
 * @tparam p_sequence_storage Per-sequence inline storage
 * @tparam p_table_storage Per-table chunk storage
 * @tparam p_index_templates Registered index templates
 */
template <class p_fragment_storage,
          class p_sequence_storage,
          class p_table_storage,
          template <class p_storage> class... p_index_templates>
requires (
    is_index_chunked_storage_for<
        p_fragment_storage, sequence_index_layout::fragment_chunks>
    && is_index_chunked_storage_for<
        p_sequence_storage, sequence_index_layout::sequence_inline>
    && is_index_chunked_storage_for<
        p_table_storage, sequence_index_layout::table_chunks>
    && sizeof...(p_index_templates) > 0)
struct supported_indexes {
    /// @brief Number of registered index templates.
    static constexpr std::size_t k_index_count =
        sizeof...(p_index_templates);

    /**
     * @brief Selects the storage type assigned to one persisted layout.
     * @tparam p_layout Persisted layout to bind
     */
    template <sequence_index_layout p_layout>
    using storage_for_type = std::conditional_t<
        p_layout == sequence_index_layout::fragment_chunks,
        p_fragment_storage,
        std::conditional_t<
            p_layout == sequence_index_layout::sequence_inline,
            p_sequence_storage,
            p_table_storage>>;
    /**
     * @brief Instantiates an index template with its assigned storage.
     * @tparam p_index_template Registered index template
     */
    template <template <class> class p_index_template>
    using bound_type = p_index_template<storage_for_type<
        index_descriptor<p_index_template>::index_type>>;

    static_assert((requires {
        index_descriptor<p_index_templates>::index_name;
        index_descriptor<p_index_templates>::index_type;
        index_descriptor<p_index_templates>::allows_update_on_insert;
    } && ...),
                  "an index template lacks a visible index_descriptor; "
                  "import its index module at this instantiation site");
    static_assert(((bound_type<p_index_templates>::index_type
                    == index_descriptor<p_index_templates>::index_type)
                   && ...),
                  "index_type drift between an index and its index_descriptor");
    static_assert(((index_for_any<bound_type<p_index_templates>>) && ...),
                  "every registered index must satisfy is_index_for for its layout");
    static_assert([]{
        for (std::string_view default_index_name
             : k_default_update_on_insert_indexes) {
            const bool is_registered =
                ((index_descriptor<p_index_templates>::index_name
                  == default_index_name)
                 || ...);
            if (!is_registered) {
                return false;
            }
        }
        return true;
    }(), "every k_default_update_on_insert_indexes entry must be registered");
    static_assert(((!is_default_update_on_insert_index(
                        index_descriptor<p_index_templates>::index_name)
                    || (index_descriptor<
                            p_index_templates>::allows_update_on_insert
                        && index_layout_allows_update_on_insert(
                            index_descriptor<
                                p_index_templates>::index_type)))
                   && ...),
                  "every default UPDATE_ON_INSERT index must allow on-insert build");

    /**
     * @brief Lists registered index names and layouts in registry order.
     * @return Fixed-size name and layout table
     */
    [[nodiscard]] static constexpr std::array<
        std::pair<std::string_view, sequence_index_layout>,
        k_index_count>
    list_types() noexcept {
        std::array<std::pair<std::string_view, sequence_index_layout>,
                   k_index_count> index_types{};
        std::size_t index_position = 0;
        ((index_types[index_position++] = {
             index_descriptor<p_index_templates>::index_name,
             index_descriptor<p_index_templates>::index_type}),
         ...);
        return index_types;
    }
    /**
     * @brief Lists insert-time maintenance eligibility in registry order.
     * @return Fixed-size eligibility table
     */
    [[nodiscard]] static constexpr std::array<bool, k_index_count>
    allows_on_insert_flags() noexcept {
        std::array<bool, k_index_count> allowed_flags{};
        std::size_t index_position = 0;
        ((allowed_flags[index_position++] =
              index_descriptor<
                  p_index_templates>::allows_update_on_insert
              && index_layout_allows_update_on_insert(
                  index_descriptor<p_index_templates>::index_type)),
         ...);
        return allowed_flags;
    }

    /// @brief Enabled state in registry order.
    std::array<bool, k_index_count> enabled{};
    /// @brief Update policy in registry order.
    std::array<index_update_policy, k_index_count> policy{};

    /**
     * @brief Verifies that a SIMD backend has its selected kernel table.
     * @param backend Selected sketch backend
     * @param kernels Immutable selected kernel table
     * @throws std::invalid_argument when a SIMD backend lacks its kernel
     */
    static void require_selected_minimizer_kernel(
            bioseq::minimizers::minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels) {
        if (backend == bioseq::minimizers::minimizer_sketch_backend::exact_simd
            && (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
            throw std::invalid_argument(
                "two_stack_nthash index build requires a selected two-stack kernel");
        }
        if (backend
                == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd
            && (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
            throw std::invalid_argument(
                "bounded_nthash_simd index build requires a selected bounded ntHash kernel");
        }
    }

    /** @brief Applies the enabled and update-policy defaults for a fresh table. */
    [[gnu::always_inline]] constexpr void apply_table_defaults() noexcept {
        for (std::size_t index_position = 0;
             index_position < k_index_count;
             ++index_position) {
            enabled[index_position] = true;
            policy[index_position] = index_update_policy::manual;
        }
        constexpr auto k_index_types = list_types();
        for (std::size_t index_position = 0;
             index_position < k_index_count;
             ++index_position) {
            if (is_default_update_on_insert_index(
                    k_index_types[index_position].first)) {
                policy[index_position] =
                    index_update_policy::update_on_insert;
            }
        }
    }
    /**
     * @brief Enables exactly the named indexes.
     * @param enabled_names Names to enable
     */
    [[gnu::always_inline]] void configure(const std::vector<std::string>& enabled_names) {
        constexpr auto k_index_types = list_types();
        for (std::size_t index_position = 0;
             index_position < k_index_count;
             ++index_position) {
            enabled[index_position] = false;
            for (const auto& enabled_name : enabled_names) {
                if (k_index_types[index_position].first == enabled_name) {
                    enabled[index_position] = true;
                    break;
                }
            }
        }
    }
    /**
     * @brief Applies persisted per-table policy rows over initialized defaults.
     * @tparam p_row_range Range whose rows expose name, enabled, and
     * update_on_insert fields
     * @param rows Persisted policy rows
     */
    template <class p_row_range>
    [[gnu::always_inline]] void merge_overrides(const p_row_range& rows) {
        constexpr auto k_index_types = list_types();
        constexpr auto k_allows_on_insert = allows_on_insert_flags();
        for (const auto& row : rows) {
            for (std::size_t index_position = 0;
                 index_position < k_index_count;
                 ++index_position) {
                if (k_index_types[index_position].first == row.name) {
                    enabled[index_position] = row.enabled;
                    policy[index_position] =
                        row.update_on_insert
                                && k_allows_on_insert[index_position]
                            ? index_update_policy::update_on_insert
                            : index_update_policy::manual;
                    break;
                }
            }
        }
    }
    /** @brief Reports the result of a single-index policy update. */
    enum class set_policy_status {
        /// @brief The requested policy was stored.
        applied,
        /// @brief No registered index matched the requested name.
        unknown_index,
        /// @brief The index does not support the requested policy.
        ineligible,
    };
    /**
     * @brief Sets the update policy of one registered index.
     * @param name Registered index name
     * @param requested_policy Policy requested by the caller
     * @return Whether the policy was applied or rejected
     */
    [[nodiscard]] set_policy_status set_policy(
            std::string_view name,
            index_update_policy requested_policy) {
        constexpr auto k_index_types = list_types();
        constexpr auto k_allows_on_insert = allows_on_insert_flags();
        for (std::size_t index_position = 0;
             index_position < k_index_count;
             ++index_position) {
            if (k_index_types[index_position].first != name) {
                continue;
            }
            if (requested_policy == index_update_policy::update_on_insert
                && !k_allows_on_insert[index_position]) {
                return set_policy_status::ineligible;
            }
            policy[index_position] = requested_policy;
            return set_policy_status::applied;
        }
        return set_policy_status::unknown_index;
    }

    /**
     * @brief Runs enabled fragmented index operations over the registry.
     * @tparam p_is_rebuild Selects rebuild instead of build
     * @tparam p_only_on_insert Restricts execution to insert-time indexes
     * @tparam p_fragment_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     * @param backend Selected sketch backend
     * @param kernels Immutable selected kernel table
     * @param alphabet_code Packed alphabet identity
     */
    template <bool p_is_rebuild,
              bool p_only_on_insert,
              class p_fragment_opener,
              sequence_alphabet p_alphabet>
    [[gnu::always_inline]] void run_fragmented(
            p_fragment_opener&& open_storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
            bioseq::minimizers::minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code) {
        std::size_t index_position = 0;
        // Explicit minimal capture keeps registry state, the fold counter,
        // callback, sequence, and backend ownership visible.
        (
            [this, &index_position, &open_storage, &sequence, &backend, &kernels, &alphabet_code] {
                using index_type = bound_type<p_index_templates>;
                if constexpr (index_type::index_type == sequence_index_layout::fragment_chunks &&
                              (!p_only_on_insert ||
                               index_descriptor<p_index_templates>::allows_update_on_insert)) {
                    static_assert(index_provides_build_fragmented<index_type, p_alphabet>);
                    static_assert(index_provides_backend_build_fragmented<index_type, p_alphabet>);
                    const std::size_t current_index_position = index_position;
                    const bool should_build =
                        enabled[current_index_position] &&
                        (!p_only_on_insert ||
                         policy[current_index_position] == index_update_policy::update_on_insert);
                    if (should_build) {
                        require_selected_minimizer_kernel(backend, kernels);
                        p_fragment_storage& storage =
                            open_storage(index_descriptor<p_index_templates>::index_name);
                        if constexpr (p_is_rebuild) {
                            index_type::rebuild(storage, sequence, backend, kernels, alphabet_code);
                        } else {
                            index_type::build(storage, sequence, backend, kernels, alphabet_code);
                        }
                    }
                }
                ++index_position;
            }(),
            ...);
    }
    /**
     * @brief Runs enabled inline index operations over the registry.
     * @tparam p_is_rebuild Selects rebuild instead of build
     * @tparam p_only_on_insert Restricts execution to insert-time indexes
     * @tparam p_inline_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     */
    template <bool p_is_rebuild,
              bool p_only_on_insert,
              class p_inline_opener,
              sequence_alphabet p_alphabet>
    [[gnu::always_inline]] void run_inline(
            p_inline_opener&& open_storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        std::size_t index_position = 0;
        (
            [this, &index_position, &open_storage, &sequence] {
                using index_type = bound_type<p_index_templates>;
                if constexpr (index_type::index_type == sequence_index_layout::sequence_inline &&
                              (!p_only_on_insert ||
                               index_descriptor<p_index_templates>::allows_update_on_insert)) {
                    static_assert(index_provides_build_inline<index_type, p_alphabet>);
                    const std::size_t current_index_position = index_position;
                    const bool should_build =
                        enabled[current_index_position] &&
                        (!p_only_on_insert ||
                         policy[current_index_position] == index_update_policy::update_on_insert);
                    if (should_build) {
                        p_sequence_storage& storage =
                            open_storage(index_descriptor<p_index_templates>::index_name);
                        if constexpr (p_is_rebuild) {
                            index_type::rebuild(storage, sequence);
                        } else {
                            index_type::build(storage, sequence);
                        }
                    }
                }
                ++index_position;
            }(),
            ...);
    }
    /**
     * @brief Runs enabled table index operations over the registry.
     * @tparam p_is_rebuild Selects rebuild instead of build
     * @tparam p_only_on_insert Restricts execution to insert-time indexes
     * @tparam p_table_opener Callable returning storage for an index name
     * @param open_storage Storage opener
     */
    template <bool p_is_rebuild,
              bool p_only_on_insert,
              class p_table_opener>
    [[gnu::always_inline]] void run_table(p_table_opener&& open_storage) {
        std::size_t index_position = 0;
        ([this, &index_position, &open_storage] {
            using index_type = bound_type<p_index_templates>;
            if constexpr (
                index_type::index_type == sequence_index_layout::table_chunks
                && !p_only_on_insert) {
                static_assert(index_provides_build_table<index_type>);
                if (enabled[index_position]) {
                    p_table_storage& storage = open_storage(
                        index_descriptor<p_index_templates>::index_name);
                    if constexpr (p_is_rebuild) {
                        index_type::rebuild(storage);
                    } else {
                        index_type::build(storage);
                    }
                }
            }
            ++index_position;
        }(), ...);
    }

    /**
     * @brief Builds enabled fragmented indexes with a selected backend.
     * @tparam p_only_on_insert Restricts execution to insert-time indexes
     * @tparam p_fragment_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     * @param backend Selected sketch backend
     * @param kernels Immutable selected kernel table
     * @param alphabet_code Packed alphabet identity
     */
    template <bool p_only_on_insert = false,
              class p_fragment_opener,
              sequence_alphabet p_alphabet>
    void build_fragmented(
        p_fragment_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        run_fragmented<false, p_only_on_insert>(
            open_storage, sequence, backend, kernels, alphabet_code);
    }
    /**
     * @brief Builds enabled fragmented indexes with the scalar backend.
     * @tparam p_only_on_insert Restricts execution to insert-time indexes
     * @tparam p_fragment_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     */
    template <bool p_only_on_insert = false,
              class p_fragment_opener,
              sequence_alphabet p_alphabet>
    void build_fragmented(
        p_fragment_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        run_fragmented<false, p_only_on_insert>(
            open_storage,
            sequence,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            nullptr,
            bioseq::minimizers::k_unbound_packed_alphabet_code);
    }
    /**
     * @brief Builds enabled per-sequence inline indexes.
     * @tparam p_only_on_insert Restricts execution to insert-time indexes
     * @tparam p_inline_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     */
    template <bool p_only_on_insert = false,
              class p_inline_opener,
              sequence_alphabet p_alphabet>
    void build_sequence(
        p_inline_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        run_inline<false, p_only_on_insert>(open_storage, sequence);
    }
    /**
     * @brief Builds enabled table-wide indexes.
     * @tparam p_table_opener Callable returning storage for an index name
     * @param open_storage Storage opener
     */
    template <class p_table_opener>
    void build_table(p_table_opener&& open_storage) {
        run_table<false, false>(open_storage);
    }
    /**
     * @brief Rebuilds enabled fragmented indexes with a selected backend.
     * @tparam p_fragment_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     * @param backend Selected sketch backend
     * @param kernels Immutable selected kernel table
     * @param alphabet_code Packed alphabet identity
     */
    template <class p_fragment_opener, sequence_alphabet p_alphabet>
    void rebuild_fragmented(
        p_fragment_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        run_fragmented<true, false>(
            open_storage, sequence, backend, kernels, alphabet_code);
    }
    /**
     * @brief Rebuilds enabled fragmented indexes with the scalar backend.
     * @tparam p_fragment_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     */
    template <class p_fragment_opener, sequence_alphabet p_alphabet>
    void rebuild_fragmented(
        p_fragment_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        run_fragmented<true, false>(
            open_storage,
            sequence,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            nullptr,
            bioseq::minimizers::k_unbound_packed_alphabet_code);
    }
    /**
     * @brief Rebuilds enabled per-sequence inline indexes.
     * @tparam p_inline_opener Callable returning storage for an index name
     * @tparam p_alphabet Sequence alphabet accepted by the indexes
     * @param open_storage Storage opener
     * @param sequence Sequence view to index
     */
    template <class p_inline_opener, sequence_alphabet p_alphabet>
    void rebuild_sequence(
        p_inline_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        run_inline<true, false>(open_storage, sequence);
    }
    /**
     * @brief Rebuilds enabled table-wide indexes.
     * @tparam p_table_opener Callable returning storage for an index name
     * @param open_storage Storage opener
     */
    template <class p_table_opener>
    void rebuild_table(p_table_opener&& open_storage) {
        run_table<true, false>(open_storage);
    }

    /**
     * @brief Finds the persisted layout of a validated index name.
     * @param name Registered index name
     * @return Persisted layout assigned to the index
     */
    [[nodiscard]] static constexpr sequence_index_layout layout_of(std::string_view name) {
        constexpr auto k_index_types = list_types();
        for (const auto& index_type : k_index_types) {
            if (index_type.first == name) {
                return index_type.second;
            }
        }
        return sequence_index_layout::fragment_chunks;
    }
    /**
     * @brief Rebuilds one named fragmented index with a selected backend.
     * @tparam p_alphabet Sequence alphabet accepted by the index
     * @param name Registered index name
     * @param storage Fragment storage for the index
     * @param sequence Sequence view to index
     * @param backend Selected sketch backend
     * @param kernels Immutable selected kernel table
     * @param alphabet_code Packed alphabet identity
     */
    template <sequence_alphabet p_alphabet>
    void rebuild_one_fragmented(
        std::string_view name,
        p_fragment_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        // Explicit minimal capture keeps task ownership visible.
        (
            [&storage, &sequence, &name, &backend, &kernels, &alphabet_code] {
                using index_type = bound_type<p_index_templates>;
                if constexpr (index_type::index_type == sequence_index_layout::fragment_chunks) {
                    if (index_descriptor<p_index_templates>::index_name == name) {
                        require_selected_minimizer_kernel(backend, kernels);
                        index_type::rebuild(storage, sequence, backend, kernels, alphabet_code);
                    }
                }
            }(),
            ...);
    }
    /**
     * @brief Rebuilds one named fragmented index with the scalar backend.
     * @tparam p_alphabet Sequence alphabet accepted by the index
     * @param name Registered index name
     * @param storage Fragment storage for the index
     * @param sequence Sequence view to index
     */
    template <sequence_alphabet p_alphabet>
    void rebuild_one_fragmented(
        std::string_view name,
        p_fragment_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        rebuild_one_fragmented(
            name,
            storage,
            sequence,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            nullptr,
            bioseq::minimizers::k_unbound_packed_alphabet_code);
    }
    /**
     * @brief Rebuilds one named per-sequence inline index.
     * @tparam p_alphabet Sequence alphabet accepted by the index
     * @param name Registered index name
     * @param storage Inline storage for the index
     * @param sequence Sequence view to index
     */
    template <sequence_alphabet p_alphabet>
    void rebuild_one_inline(
        std::string_view name,
        p_sequence_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        (
            [&name, &storage, &sequence] {
                using index_type = bound_type<p_index_templates>;
                if constexpr (index_type::index_type == sequence_index_layout::sequence_inline) {
                    if (index_descriptor<p_index_templates>::index_name == name) {
                        index_type::rebuild(storage, sequence);
                    }
                }
            }(),
            ...);
    }
    /**
     * @brief Rebuilds one named table-wide index.
     * @param name Registered index name
     * @param storage Table storage for the index
     */
    void rebuild_one_table(
            std::string_view name,
            p_table_storage& storage) {
        ([&name, &storage] {
            using index_type = bound_type<p_index_templates>;
            if constexpr (
                index_type::index_type
                == sequence_index_layout::table_chunks) {
                if (index_descriptor<p_index_templates>::index_name == name) {
                    index_type::rebuild(storage);
                }
            }
        }(), ...);
    }

    /**
     * @brief Clears all persisted chunks of one named index.
     * @param fragment_storage Fragment storage provider
     * @param sequence_storage Inline storage provider
     * @param table_storage Table storage provider
     * @param name Registered index name
     */
    void clear_index(
            p_fragment_storage& fragment_storage,
            p_sequence_storage& sequence_storage,
            p_table_storage& table_storage,
            std::string_view name) {
        ([&fragment_storage,
          &sequence_storage,
          &table_storage,
          &name] {
            constexpr sequence_index_layout k_layout =
                index_descriptor<p_index_templates>::index_type;
            if (index_descriptor<p_index_templates>::index_name == name) {
                if constexpr (
                    k_layout == sequence_index_layout::fragment_chunks) {
                    fragment_storage.clear(name);
                } else if constexpr (
                    k_layout == sequence_index_layout::sequence_inline) {
                    sequence_storage.clear(name);
                } else if constexpr (
                    k_layout == sequence_index_layout::table_chunks) {
                    table_storage.clear(name);
                }
            }
        }(), ...);
    }
};
} // namespace bioseq::indexing
