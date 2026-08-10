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

template <class StorageFragment, class StorageSequence, class StorageTable,
          template <class S> class... Indexes>
requires (   is_index_chunked_storage_for<StorageFragment, sequence_index_layout::fragment_chunks>
          && is_index_chunked_storage_for<StorageSequence, sequence_index_layout::sequence_inline>
          && is_index_chunked_storage_for<StorageTable,    sequence_index_layout::table_chunks>
          && sizeof...(Indexes) > 0)
struct supported_indexes {
    static constexpr std::size_t k_index_count = sizeof...(Indexes);

    // Pick the concrete storage type for a layout (compile-time).
    template <sequence_index_layout M>
    using storage_for_t =
        std::conditional_t<M == sequence_index_layout::fragment_chunks, StorageFragment,
        std::conditional_t<M == sequence_index_layout::sequence_inline,  StorageSequence,
                                                                         StorageTable>>;
    // Bind each index template to its layout's storage.
    template <template <class> class I>
    using bound_t = I<storage_for_t<index_descriptor<I>::index_type>>;

    static_assert((requires { index_descriptor<Indexes>::index_name;
                              index_descriptor<Indexes>::index_type;
                              index_descriptor<Indexes>::allows_update_on_insert; } && ...),
                  "supported_indexes<...>: an index template has no VISIBLE index_descriptor specialization — "
                  "import its module (e.g. bioseq_indexing.minimizer_hashmap / .minimizer_sequence / .symbol_counts) "
                  "at THIS instantiation site");
    static_assert(((bound_t<Indexes>::index_type == index_descriptor<Indexes>::index_type) && ...),
                  "index_type drift between an index and its index_descriptor");
    static_assert(((index_for_any<bound_t<Indexes>>) && ...),
                  "every registered index must satisfy is_index_for for its layout");
    static_assert([]{
        for (std::string_view d : k_default_update_on_insert_indexes) {
            bool found = ((index_descriptor<Indexes>::index_name == d) || ...);
            if (!found) return false;
        }
        return true;
    }(), "every k_default_update_on_insert_indexes entry must be registered");
    static_assert(((!is_default_update_on_insert_index(index_descriptor<Indexes>::index_name)
                    || (index_descriptor<Indexes>::allows_update_on_insert
                        && index_layout_allows_update_on_insert(index_descriptor<Indexes>::index_type))) && ...),
                  "every default UPDATE_ON_INSERT index must allow on-insert build");

    /// All (name, layout) pairs — consumed by metadata_admin to validate names.
    [[nodiscard]] static constexpr std::array<std::pair<std::string_view, sequence_index_layout>, k_index_count>
    list_types() noexcept {
        std::array<std::pair<std::string_view, sequence_index_layout>, k_index_count> out{};
        std::size_t i = 0;
        ((out[i++] = { index_descriptor<Indexes>::index_name,
                       index_descriptor<Indexes>::index_type }), ...);
        return out;
    }
    /// Per-index UPDATE_ON_INSERT eligibility (parallel to list_types).
    [[nodiscard]] static constexpr std::array<bool, k_index_count> allows_on_insert_flags() noexcept {
        std::array<bool, k_index_count> out{}; std::size_t i = 0;
        ((out[i++] = index_descriptor<Indexes>::allows_update_on_insert
                     && index_layout_allows_update_on_insert(index_descriptor<Indexes>::index_type)), ...);
        return out;
    }

    // ---- per-table runtime enable set + update policy -------------------
    std::array<bool, k_index_count> enabled{};
    std::array<index_update_policy, k_index_count> policy{};

    static void require_selected_minimizer_kernel_(
            bioseq::minimizers::minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels) {
        if (backend == bioseq::minimizers::minimizer_sketch_backend::two_stack_nthash
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
        for (std::size_t i = 0; i < k_index_count; ++i) { enabled[i] = true; policy[i] = index_update_policy::manual; }
        constexpr auto names = list_types();
        for (std::size_t i = 0; i < k_index_count; ++i)
            if (is_default_update_on_insert_index(names[i].first)) policy[i] = index_update_policy::update_on_insert;
    }
    /// Enable exactly the named indexes (others disabled).
    [[gnu::always_inline]] void configure(const std::vector<std::string>& enabled_names) {
        constexpr auto names = list_types();
        for (std::size_t i = 0; i < k_index_count; ++i) {
            enabled[i] = false;
            for (const auto& n : enabled_names) if (names[i].first == n) { enabled[i] = true; break; }
        }
    }
    /// Overlay persisted per-table policy rows onto the defaults. Call
    /// apply_table_defaults() FIRST, then merge_overrides.
    /// `Row` = {std::string_view name; bool enabled; bool update_on_insert;}.
    template <class RowRange>
    [[gnu::always_inline]] void merge_overrides(const RowRange& rows) {
        constexpr auto types = list_types(); constexpr auto allows = allows_on_insert_flags();
        for (const auto& r : rows)
            for (std::size_t i = 0; i < k_index_count; ++i) if (types[i].first == r.name) {
                enabled[i] = r.enabled;
                policy[i]  = (r.update_on_insert && allows[i])
                           ? index_update_policy::update_on_insert : index_update_policy::manual;
                break;
            }
    }
    /// Set a single index's update policy. Returns a status (the eligibility
    /// guard lives here in one place).
    enum class set_policy_status { applied, unknown_index, ineligible };
    [[nodiscard]] set_policy_status set_policy(std::string_view name, index_update_policy p) {
        constexpr auto types = list_types(); constexpr auto allows = allows_on_insert_flags();
        for (std::size_t i = 0; i < k_index_count; ++i) if (types[i].first == name) {
            if (p == index_update_policy::update_on_insert && !allows[i]) {
                return set_policy_status::ineligible;
            }
            policy[i] = p;
            return set_policy_status::applied;
        }
        return set_policy_status::unknown_index;
    }

    // ---- build / rebuild drivers (fold over the index pack) -------------
    // Fragmented build/rebuild receives the one already-selected minimizer
    // table and forwards it unchanged to every selected index implementation.
    template <bool Rebuild, bool OnlyOnInsert, class OpenFragment,
              sequence_alphabet Alphabet>
    [[gnu::always_inline]] void run_fragmented_(OpenFragment&& open_for,
            bioseq::storage::lazy_sequence_view<Alphabet> seq,
            bioseq::minimizers::minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code) {
        std::size_t i = 0;
        // Explicit minimal capture keeps registry state, the fold counter,
        // callback, sequence, and backend ownership visible.
        ([this, &i, &open_for, seq, backend, kernels, alphabet_code]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == sequence_index_layout::fragment_chunks &&
                          (!OnlyOnInsert || index_descriptor<Indexes>::allows_update_on_insert)) {
                static_assert(index_provides_build_fragmented<B, Alphabet>);
                static_assert(index_provides_backend_build_fragmented<
                    B, Alphabet>);
                const std::size_t idx = i;
                const bool want = enabled[idx] &&
                    (!OnlyOnInsert || policy[idx] == index_update_policy::update_on_insert);
                if (want) {
                    require_selected_minimizer_kernel_(backend, kernels);
                    StorageFragment& storage =
                        open_for(index_descriptor<Indexes>::index_name);
                    if constexpr (Rebuild) {
                        B::rebuild(
                            storage, seq, backend, kernels, alphabet_code);
                    } else {
                        B::build(
                            storage, seq, backend, kernels, alphabet_code);
                    }
                }
            }
            ++i;
        }(), ...);
    }
    template <bool Rebuild, bool OnlyOnInsert, class OpenInline,
              sequence_alphabet Alphabet>
    [[gnu::always_inline]] void run_inline_(OpenInline&& open_for,
            bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        std::size_t i = 0;
        ([this, &i, &open_for, seq]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == sequence_index_layout::sequence_inline &&
                          (!OnlyOnInsert || index_descriptor<Indexes>::allows_update_on_insert)) {
                static_assert(index_provides_build_inline<B, Alphabet>);
                const std::size_t idx = i;
                const bool want = enabled[idx] &&
                    (!OnlyOnInsert || policy[idx] == index_update_policy::update_on_insert);
                if (want) { StorageSequence& s = open_for(index_descriptor<Indexes>::index_name);
                            if constexpr (Rebuild) B::rebuild(s, seq); else B::build(s, seq); }
            }
            ++i;
        }(), ...);
    }
    template <bool Rebuild, bool OnlyOnInsert, class OpenTable>
    [[gnu::always_inline]] void run_table_(OpenTable&& open_for) {
        std::size_t i = 0;
        ([this, &i, &open_for]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == sequence_index_layout::table_chunks && !OnlyOnInsert) {
                static_assert(index_provides_build_table<B>);
                if (enabled[i]) { StorageTable& s = open_for(index_descriptor<Indexes>::index_name);
                                  if constexpr (Rebuild) B::rebuild(s); else B::build(s); }
            }
            ++i;
        }(), ...);
    }

    // Explicit-backend and scalar-default fragmented drivers.
    template <bool OnlyOnInsert = false, class OpenFragment,
              sequence_alphabet Alphabet>
    void build_fragmented(
        OpenFragment&& open_for,
        bioseq::storage::lazy_sequence_view<Alphabet> seq,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        run_fragmented_<false, OnlyOnInsert>(
            open_for, seq, backend, kernels, alphabet_code);
    }
    template <bool OnlyOnInsert = false, class OpenFragment,
              sequence_alphabet Alphabet>
    void build_fragmented(
        OpenFragment&& open_for,
        bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        run_fragmented_<false, OnlyOnInsert>(
            open_for,
            seq,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            nullptr,
            bioseq::minimizers::unbound_packed_alphabet_code);
    }
    template <bool OnlyOnInsert=false, class OpenInline,
              sequence_alphabet Alphabet>
    void build_sequence(
        OpenInline&& open_for,
        bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        run_inline_<false, OnlyOnInsert>(open_for, seq);
    }
    template <class OpenTable> void build_table(OpenTable&& open_for) { run_table_<false, false>(open_for); }
    template <class OpenFragment, sequence_alphabet Alphabet>
    void rebuild_fragmented(
        OpenFragment&& open_for,
        bioseq::storage::lazy_sequence_view<Alphabet> seq,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels,
        std::uint8_t alphabet_code) {
        run_fragmented_<true, false>(
            open_for, seq, backend, kernels, alphabet_code);
    }
    template <class OpenFragment, sequence_alphabet Alphabet>
    void rebuild_fragmented(
        OpenFragment&& open_for,
        bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        run_fragmented_<true, false>(
            open_for,
            seq,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            nullptr,
            bioseq::minimizers::unbound_packed_alphabet_code);
    }
    template <class OpenInline, sequence_alphabet Alphabet>
    void rebuild_sequence(
        OpenInline&& open_for,
        bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        run_inline_<true, false>(open_for, seq);
    }
    template <class OpenTable> void rebuild_table(OpenTable&& open_for) { run_table_<true, false>(open_for); }

    /// Layout of a named index (compile-time table lookup), for the SQL surface to route by.
    [[nodiscard]] static constexpr sequence_index_layout layout_of(std::string_view name) {
        constexpr auto types = list_types();
        for (auto& t : types) if (t.first == name) return t.second;
        return sequence_index_layout::fragment_chunks;   // unreachable: caller validated name
    }
    /// Rebuild ONLY the named index for ONE sequence (per-sequence layouts).
    /// Explicit-backend and scalar-default forms.
    template <sequence_alphabet Alphabet>
    void rebuild_one_fragmented(
        std::string_view name,
        StorageFragment& storage,
            bioseq::storage::lazy_sequence_view<Alphabet> seq,
            bioseq::minimizers::minimizer_sketch_backend backend,
            const bioseq::minimizers::minimizer_kernels* kernels,
            std::uint8_t alphabet_code) {
        // Explicit minimal capture keeps task ownership visible.
        ([&storage, seq, &name, backend, kernels, alphabet_code]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == sequence_index_layout::fragment_chunks) {
                if (index_descriptor<Indexes>::index_name == name) {
                    require_selected_minimizer_kernel_(backend, kernels);
                    B::rebuild(
                        storage, seq, backend, kernels, alphabet_code);
                }
            }
        }(), ...);
    }
    template <sequence_alphabet Alphabet>
    void rebuild_one_fragmented(
        std::string_view name,
        StorageFragment& storage,
        bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        rebuild_one_fragmented(
            name,
            storage,
            seq,
            bioseq::minimizers::minimizer_sketch_backend::scalar,
            nullptr,
            bioseq::minimizers::unbound_packed_alphabet_code);
    }
    template <sequence_alphabet Alphabet>
    void rebuild_one_inline(
        std::string_view name,
        StorageSequence& s,
        bioseq::storage::lazy_sequence_view<Alphabet> seq) {
        ([&name, &s, seq]{ using B = bound_t<Indexes>;
            if constexpr (B::index_type == sequence_index_layout::sequence_inline)
                if (index_descriptor<Indexes>::index_name == name) B::rebuild(s, seq);
        }(), ...);
    }
    void rebuild_one_table(std::string_view name, StorageTable& s) {
        ([&name, &s]{ using B = bound_t<Indexes>;
            if constexpr (B::index_type == sequence_index_layout::table_chunks)
                if (index_descriptor<Indexes>::index_name == name) B::rebuild(s);
        }(), ...);
    }

    /// Drop every chunk of one index. Delegates to whichever storage backs that index.
    void clear_index(StorageFragment& f, StorageSequence& q, StorageTable& t, std::string_view name) {
        ([&f, &q, &t, &name]{
            constexpr sequence_index_layout M = index_descriptor<Indexes>::index_type;
            if (index_descriptor<Indexes>::index_name == name) {
                if constexpr      (M == sequence_index_layout::fragment_chunks) f.clear(name);
                else if constexpr (M == sequence_index_layout::sequence_inline)  q.clear(name);
                else if constexpr (M == sequence_index_layout::table_chunks)    t.clear(name);
            }
        }(), ...);
    }
};
}  // namespace bioseq::indexing
