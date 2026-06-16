/**
 * @file supported_indexes.cppm
 * @brief DOC0279 §7.3 — the compile-time `supported_indexes<...>` registry.
 *
 * The registry is the substrate the indexes build on; it does NOT import the
 * index modules. Every TU that instantiates `supported_indexes<...>` must import
 * the index modules whose descriptors it reads (the §7.3 static_assert turns a
 * forgotten import into a diagnosed error). NO_POSTGRES.
 */
module;
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.indexing.registry;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;          // the index_descriptor PRIMARY (declared-undefined)
import bioseq.storage.lazy;             // lazy_compact_sequence<R,Scanner>
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

template <class StorageFragment, class StorageSequence, class StorageTable,
          template <class S> class... Indexes>
requires (   is_index_chunked_storage_for<StorageFragment, SequenceIndexType::CHUNKED_PER_FRAGMENT>
          && is_index_chunked_storage_for<StorageSequence, SequenceIndexType::INLINE_PER_SEQUENCE>
          && is_index_chunked_storage_for<StorageTable,    SequenceIndexType::CHUNKED_PER_TABLE>
          && sizeof...(Indexes) > 0)
struct supported_indexes {
    static constexpr std::size_t kIndexCount = sizeof...(Indexes);

    // Pick the concrete storage type for a layout (compile-time).
    template <SequenceIndexType M>
    using storage_for_t =
        std::conditional_t<M == SequenceIndexType::CHUNKED_PER_FRAGMENT, StorageFragment,
        std::conditional_t<M == SequenceIndexType::INLINE_PER_SEQUENCE,  StorageSequence,
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
        for (std::string_view d : DEFAULT_UPDATE_ON_INSERT_INDEXES) {
            bool found = ((index_descriptor<Indexes>::index_name == d) || ...);
            if (!found) return false;
        }
        return true;
    }(), "every DEFAULT_UPDATE_ON_INSERT_INDEXES entry must be registered");
    static_assert(((!default_update_on_insert_contains(index_descriptor<Indexes>::index_name)
                    || (index_descriptor<Indexes>::allows_update_on_insert
                        && index_type_allows_update_on_insert(index_descriptor<Indexes>::index_type))) && ...),
                  "every default UPDATE_ON_INSERT index must allow on-insert build");

    /// All (name, layout) pairs — consumed by metadata_admin to validate names.
    [[nodiscard]] static constexpr std::array<std::pair<std::string_view, SequenceIndexType>, kIndexCount>
    list_types() noexcept {
        std::array<std::pair<std::string_view, SequenceIndexType>, kIndexCount> out{};
        std::size_t i = 0;
        ((out[i++] = { index_descriptor<Indexes>::index_name,
                       index_descriptor<Indexes>::index_type }), ...);
        return out;
    }
    /// Per-index UPDATE_ON_INSERT eligibility (parallel to list_types).
    [[nodiscard]] static constexpr std::array<bool, kIndexCount> allows_on_insert_flags() noexcept {
        std::array<bool, kIndexCount> out{}; std::size_t i = 0;
        ((out[i++] = index_descriptor<Indexes>::allows_update_on_insert
                     && index_type_allows_update_on_insert(index_descriptor<Indexes>::index_type)), ...);
        return out;
    }

    // ---- per-table runtime enable set + update policy -------------------
    std::array<bool, kIndexCount>              enabled{};
    std::array<IndexUpdatePolicy, kIndexCount> policy{};

    /// Default a fresh table.
    [[gnu::always_inline]] constexpr void apply_table_defaults() noexcept {
        for (std::size_t i = 0; i < kIndexCount; ++i) { enabled[i] = true; policy[i] = IndexUpdatePolicy::UPDATE_MANUALLY; }
        constexpr auto names = list_types();
        for (std::size_t i = 0; i < kIndexCount; ++i)
            if (default_update_on_insert_contains(names[i].first)) policy[i] = IndexUpdatePolicy::UPDATE_ON_INSERT;
    }
    /// Enable exactly the named indexes (others disabled).
    [[gnu::always_inline]] void configure(const std::vector<std::string>& enabled_names) {
        constexpr auto names = list_types();
        for (std::size_t i = 0; i < kIndexCount; ++i) {
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
            for (std::size_t i = 0; i < kIndexCount; ++i) if (types[i].first == r.name) {
                enabled[i] = r.enabled;
                policy[i]  = (r.update_on_insert && allows[i])
                           ? IndexUpdatePolicy::UPDATE_ON_INSERT : IndexUpdatePolicy::UPDATE_MANUALLY;
                break;
            }
    }
    /// Set a single index's update policy. Returns a status (the eligibility
    /// guard lives here in one place).
    enum class SetPolicyStatus { Ok, Unknown, Ineligible };
    [[nodiscard]] SetPolicyStatus set_policy(std::string_view name, IndexUpdatePolicy p) {
        constexpr auto types = list_types(); constexpr auto allows = allows_on_insert_flags();
        for (std::size_t i = 0; i < kIndexCount; ++i) if (types[i].first == name) {
            if (p == IndexUpdatePolicy::UPDATE_ON_INSERT && !allows[i]) return SetPolicyStatus::Ineligible;
            policy[i] = p; return SetPolicyStatus::Ok;
        }
        return SetPolicyStatus::Unknown;
    }

    // ---- build / rebuild drivers (fold over the index pack) -------------
    template <bool Rebuild, bool OnlyOnInsert, class OpenFragment, class R, class Scanner>
    [[gnu::always_inline]] void run_fragmented_(OpenFragment&& open_for,
            bioseq::storage::lazy_compact_sequence<R, Scanner>& seq) {
        std::size_t i = 0;
        ([&]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == SequenceIndexType::CHUNKED_PER_FRAGMENT &&
                          (!OnlyOnInsert || index_descriptor<Indexes>::allows_update_on_insert)) {
                static_assert(index_provides_build_fragmented<B, R, Scanner>);
                const std::size_t idx = i;
                const bool want = enabled[idx] &&
                    (!OnlyOnInsert || policy[idx] == IndexUpdatePolicy::UPDATE_ON_INSERT);
                if (want) { StorageFragment& s = open_for(index_descriptor<Indexes>::index_name);
                            if constexpr (Rebuild) B::rebuild(s, seq); else B::build(s, seq); }
            }
            ++i;
        }(), ...);
    }
    template <bool Rebuild, bool OnlyOnInsert, class OpenInline, class R, class Scanner>
    [[gnu::always_inline]] void run_inline_(OpenInline&& open_for,
            bioseq::storage::lazy_compact_sequence<R, Scanner>& seq) {
        std::size_t i = 0;
        ([&]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == SequenceIndexType::INLINE_PER_SEQUENCE &&
                          (!OnlyOnInsert || index_descriptor<Indexes>::allows_update_on_insert)) {
                static_assert(index_provides_build_inline<B, R, Scanner>);
                const std::size_t idx = i;
                const bool want = enabled[idx] &&
                    (!OnlyOnInsert || policy[idx] == IndexUpdatePolicy::UPDATE_ON_INSERT);
                if (want) { StorageSequence& s = open_for(index_descriptor<Indexes>::index_name);
                            if constexpr (Rebuild) B::rebuild(s, seq); else B::build(s, seq); }
            }
            ++i;
        }(), ...);
    }
    template <bool Rebuild, bool OnlyOnInsert, class OpenTable>
    [[gnu::always_inline]] void run_table_(OpenTable&& open_for) {
        std::size_t i = 0;
        ([&]{
            using B = bound_t<Indexes>;
            if constexpr (B::index_type == SequenceIndexType::CHUNKED_PER_TABLE && !OnlyOnInsert) {
                static_assert(index_provides_build_table<B>);
                if (enabled[i]) { StorageTable& s = open_for(index_descriptor<Indexes>::index_name);
                                  if constexpr (Rebuild) B::rebuild(s); else B::build(s); }
            }
            ++i;
        }(), ...);
    }

    template <bool OnlyOnInsert=false, class OpenFragment, class R, class Scanner>
    void build_fragmented(OpenFragment&& open_for, bioseq::storage::lazy_compact_sequence<R,Scanner>& seq) { run_fragmented_<false, OnlyOnInsert>(open_for, seq); }
    template <bool OnlyOnInsert=false, class OpenInline, class R, class Scanner>
    void build_sequence(OpenInline&& open_for, bioseq::storage::lazy_compact_sequence<R,Scanner>& seq) { run_inline_<false, OnlyOnInsert>(open_for, seq); }
    template <class OpenTable> void build_table(OpenTable&& open_for) { run_table_<false, false>(open_for); }
    template <class OpenFragment, class R, class Scanner> void rebuild_fragmented(OpenFragment&& open_for, bioseq::storage::lazy_compact_sequence<R,Scanner>& seq) { run_fragmented_<true, false>(open_for, seq); }
    template <class OpenInline,   class R, class Scanner> void rebuild_sequence  (OpenInline&&   open_for, bioseq::storage::lazy_compact_sequence<R,Scanner>& seq) { run_inline_<true, false>(open_for, seq); }
    template <class OpenTable> void rebuild_table(OpenTable&& open_for) { run_table_<true, false>(open_for); }

    /// Layout of a named index (compile-time table lookup), for the SQL surface to route by.
    [[nodiscard]] static constexpr SequenceIndexType layout_of(std::string_view name) {
        constexpr auto types = list_types();
        for (auto& t : types) if (t.first == name) return t.second;
        return SequenceIndexType::CHUNKED_PER_FRAGMENT;   // unreachable: caller validated name
    }
    /// Rebuild ONLY the named index for ONE sequence (per-sequence layouts).
    template <class R, class Scanner>
    void rebuild_one_fragmented(std::string_view name, StorageFragment& s, bioseq::storage::lazy_compact_sequence<R,Scanner>& seq) {
        ([&]{ using B = bound_t<Indexes>;
            if constexpr (B::index_type == SequenceIndexType::CHUNKED_PER_FRAGMENT)
                if (index_descriptor<Indexes>::index_name == name) B::rebuild(s, seq);
        }(), ...);
    }
    template <class R, class Scanner>
    void rebuild_one_inline(std::string_view name, StorageSequence& s, bioseq::storage::lazy_compact_sequence<R,Scanner>& seq) {
        ([&]{ using B = bound_t<Indexes>;
            if constexpr (B::index_type == SequenceIndexType::INLINE_PER_SEQUENCE)
                if (index_descriptor<Indexes>::index_name == name) B::rebuild(s, seq);
        }(), ...);
    }
    void rebuild_one_table(std::string_view name, StorageTable& s) {
        ([&]{ using B = bound_t<Indexes>;
            if constexpr (B::index_type == SequenceIndexType::CHUNKED_PER_TABLE)
                if (index_descriptor<Indexes>::index_name == name) B::rebuild(s);
        }(), ...);
    }

    /// Drop every chunk of one index. Delegates to whichever storage backs that index.
    void clear_index(StorageFragment& f, StorageSequence& q, StorageTable& t, std::string_view name) {
        ([&]{
            constexpr SequenceIndexType M = index_descriptor<Indexes>::index_type;
            if (index_descriptor<Indexes>::index_name == name) {
                if constexpr      (M == SequenceIndexType::CHUNKED_PER_FRAGMENT) f.clear(name);
                else if constexpr (M == SequenceIndexType::INLINE_PER_SEQUENCE)  q.clear(name);
                else if constexpr (M == SequenceIndexType::CHUNKED_PER_TABLE)    t.clear(name);
            }
        }(), ...);
    }
};
}  // namespace bioseq::indexing
