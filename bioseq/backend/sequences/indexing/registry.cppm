/**
 * @file registry.cppm
 * @brief Compile-time registry for supported sequence indexes.
 *
 * The registry is the substrate the indexes build on; it does NOT import the
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
import bioseq.indexing.traits;          // the index_descriptor PRIMARY (declared-undefined)
import bioseq.sequences;
import bioseq.storage.lazy_sequence;             // canonical lazy_sequence_view
import bioseq.sequences.kmers.highway_minimizer_contract;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

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
    static constexpr std::size_t k_index_count =
        sizeof...(p_index_templates);

    // Pick the concrete storage type for a layout (compile-time).
    template <sequence_index_layout p_layout>
    using storage_for_type = std::conditional_t<
        p_layout == sequence_index_layout::fragment_chunks,
        p_fragment_storage,
        std::conditional_t<
            p_layout == sequence_index_layout::sequence_inline,
            p_sequence_storage,
            p_table_storage>>;
    // Bind each index template to its layout's storage.
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
            if (!is_registered) return false;
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

    /// All (name, layout) pairs — consumed by metadata_admin to validate names.
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
    /// Per-index UPDATE_ON_INSERT eligibility (parallel to list_types).
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

    // ---- per-table runtime enable set + update policy -------------------
    std::array<bool, k_index_count> enabled{};
    std::array<index_update_policy, k_index_count> policy{};

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

    /// Default a fresh table.
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
    /// Enable exactly the named indexes (others disabled).
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
    /// Overlay persisted per-table policy rows onto the defaults. Call
    /// apply_table_defaults() FIRST, then merge_overrides.
    /// `Row` = {std::string_view name; bool enabled; bool update_on_insert;}.
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
    /// Set a single index's update policy. Returns a status (the eligibility
    /// guard lives here in one place).
    enum class set_policy_status { applied, unknown_index, ineligible };
    [[nodiscard]] set_policy_status set_policy(
            std::string_view name,
            index_update_policy requested_policy) {
        constexpr auto k_index_types = list_types();
        constexpr auto k_allows_on_insert = allows_on_insert_flags();
        for (std::size_t index_position = 0;
             index_position < k_index_count;
             ++index_position) {
            if (k_index_types[index_position].first != name) continue;
            if (requested_policy == index_update_policy::update_on_insert
                && !k_allows_on_insert[index_position]) {
                return set_policy_status::ineligible;
            }
            policy[index_position] = requested_policy;
            return set_policy_status::applied;
        }
        return set_policy_status::unknown_index;
    }

    // ---- build / rebuild drivers (fold over the index pack) -------------
    // Fragmented build/rebuild receives the one already-selected minimizer
    // table and forwards it unchanged to every selected index implementation.
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
        ([this,
          &index_position,
          &open_storage,
          sequence,
          backend,
          kernels,
          alphabet_code] {
            using index_type = bound_type<p_index_templates>;
            if constexpr (
                index_type::index_type
                    == sequence_index_layout::fragment_chunks
                && (!p_only_on_insert
                    || index_descriptor<
                        p_index_templates>::allows_update_on_insert)) {
                static_assert(index_provides_build_fragmented<
                              index_type, p_alphabet>);
                static_assert(index_provides_backend_build_fragmented<
                    index_type, p_alphabet>);
                const std::size_t current_index_position = index_position;
                const bool should_build = enabled[current_index_position]
                    && (!p_only_on_insert
                        || policy[current_index_position]
                            == index_update_policy::update_on_insert);
                if (should_build) {
                    require_selected_minimizer_kernel(backend, kernels);
                    p_fragment_storage& storage = open_storage(
                        index_descriptor<p_index_templates>::index_name);
                    if constexpr (p_is_rebuild) {
                        index_type::rebuild(
                            storage,
                            sequence,
                            backend,
                            kernels,
                            alphabet_code);
                    } else {
                        index_type::build(
                            storage,
                            sequence,
                            backend,
                            kernels,
                            alphabet_code);
                    }
                }
            }
            ++index_position;
        }(), ...);
    }
    template <bool p_is_rebuild,
              bool p_only_on_insert,
              class p_inline_opener,
              sequence_alphabet p_alphabet>
    [[gnu::always_inline]] void run_inline(
            p_inline_opener&& open_storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        std::size_t index_position = 0;
        ([this, &index_position, &open_storage, sequence] {
            using index_type = bound_type<p_index_templates>;
            if constexpr (
                index_type::index_type
                    == sequence_index_layout::sequence_inline
                && (!p_only_on_insert
                    || index_descriptor<
                        p_index_templates>::allows_update_on_insert)) {
                static_assert(index_provides_build_inline<
                              index_type, p_alphabet>);
                const std::size_t current_index_position = index_position;
                const bool should_build = enabled[current_index_position]
                    && (!p_only_on_insert
                        || policy[current_index_position]
                            == index_update_policy::update_on_insert);
                if (should_build) {
                    p_sequence_storage& storage = open_storage(
                        index_descriptor<p_index_templates>::index_name);
                    if constexpr (p_is_rebuild) {
                        index_type::rebuild(storage, sequence);
                    } else {
                        index_type::build(storage, sequence);
                    }
                }
            }
            ++index_position;
        }(), ...);
    }
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

    // Explicit-backend and scalar-default fragmented drivers.
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
    template <bool p_only_on_insert = false,
              class p_inline_opener,
              sequence_alphabet p_alphabet>
    void build_sequence(
        p_inline_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        run_inline<false, p_only_on_insert>(open_storage, sequence);
    }
    template <class p_table_opener>
    void build_table(p_table_opener&& open_storage) {
        run_table<false, false>(open_storage);
    }
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
    template <class p_inline_opener, sequence_alphabet p_alphabet>
    void rebuild_sequence(
        p_inline_opener&& open_storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        run_inline<true, false>(open_storage, sequence);
    }
    template <class p_table_opener>
    void rebuild_table(p_table_opener&& open_storage) {
        run_table<true, false>(open_storage);
    }

    /// Layout of a named index (compile-time table lookup), for the SQL surface to route by.
    [[nodiscard]] static constexpr sequence_index_layout layout_of(std::string_view name) {
        constexpr auto k_index_types = list_types();
        for (const auto& index_type : k_index_types) {
            if (index_type.first == name) return index_type.second;
        }
        return sequence_index_layout::fragment_chunks;   // unreachable: caller validated name
    }
    /// Rebuild ONLY the named index for ONE sequence (per-sequence layouts).
    /// Explicit-backend and scalar-default forms.
    template <sequence_alphabet p_alphabet>
    void rebuild_one_fragmented(
        std::string_view name,
        p_fragment_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        // Explicit minimal capture keeps task ownership visible.
        ([&storage, sequence, &name, backend, kernels, alphabet_code] {
            using index_type = bound_type<p_index_templates>;
            if constexpr (
                index_type::index_type
                == sequence_index_layout::fragment_chunks) {
                if (index_descriptor<p_index_templates>::index_name == name) {
                    require_selected_minimizer_kernel(backend, kernels);
                    index_type::rebuild(
                        storage,
                        sequence,
                        backend,
                        kernels,
                        alphabet_code);
                }
            }
        }(), ...);
    }
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
    template <sequence_alphabet p_alphabet>
    void rebuild_one_inline(
        std::string_view name,
        p_sequence_storage& storage,
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
        ([&name, &storage, sequence] {
            using index_type = bound_type<p_index_templates>;
            if constexpr (
                index_type::index_type
                == sequence_index_layout::sequence_inline) {
                if (index_descriptor<p_index_templates>::index_name == name) {
                    index_type::rebuild(storage, sequence);
                }
            }
        }(), ...);
    }
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

    /// Drop every chunk of one index. Delegates to whichever storage backs that index.
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
}  // namespace bioseq::indexing
