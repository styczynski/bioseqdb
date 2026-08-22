/**
 * @file scanner/lazy_binding_contract.cppm
 * @brief Defines the immutable PostgreSQL lazy-sequence source binding.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.scanner.lazy_binding_contract;

import std;
import bioseq.postgres.storage.manager;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.storage.layout;
import bioseq.storage.scanner;

export namespace bioseq::storage {

/**
 * Immutable PostgreSQL relation, locator, and lifetime resources from which
 * one callback-scoped lazy facade may be constructed.
 *
 * Keeping this value separate from the facade lets administrative writers
 * validate publication inputs without importing the concrete scanner.
 * @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
 */
template <class p_sequence_config> class postgres_lazy_sequence_binding final {
  public:
    /// Concrete sequence representation captured by this binding.
    using sequence_config = p_sequence_config;
    /// Logical alphabet exposed by callback-scoped views.
    using alphabet_type = typename compact_sequence_range<p_sequence_config>::traits::alphabet_type;

    /// Capture a validated locator and borrowed PostgreSQL read resources.
    /// @param db_oid Database component of the source identity.
    /// @param tablespace_oid Tablespace component of the source identity.
    /// @param main_relfilenode File component of the source identity.
    /// @param column_attno One-based sequence attribute number.
    /// @param locator Physical fragment range and logical sequence length.
    /// @param last_fragment_padding Unused letters in the final packed unit.
    /// @param registered_snapshot Borrowed registered MVCC snapshot.
    /// @param caches Borrowed read-side cache.
    /// @param parent_mctx Parent context for callback-scoped allocation.
    postgres_lazy_sequence_binding(Oid db_oid,
                                   Oid tablespace_oid,
                                   Oid main_relfilenode,
                                   std::int16_t column_attno,
                                   const sequence_locator& locator,
                                   std::size_t last_fragment_padding,
                                   Snapshot registered_snapshot,
                                   seq_caches* caches,
                                   MemoryContext parent_mctx) noexcept
        : db_oid_(db_oid), tablespace_oid_(tablespace_oid), main_relfilenode_(main_relfilenode),
          column_attno_(column_attno), locator_(locator), trailing_padding_(last_fragment_padding),
          registered_snapshot_(registered_snapshot), caches_(caches), parent_mctx_(parent_mctx) {}

    postgres_lazy_sequence_binding(const postgres_lazy_sequence_binding&) noexcept = default;
    postgres_lazy_sequence_binding& operator=(const postgres_lazy_sequence_binding&) = delete;

    /// @return Database component of the source identity.
    [[nodiscard]] Oid db_oid() const noexcept {
        return db_oid_;
    }
    /// @return Tablespace component of the source identity.
    [[nodiscard]] Oid tablespace_oid() const noexcept {
        return tablespace_oid_;
    }
    /// @return File component of the source identity.
    [[nodiscard]] Oid main_relfilenode() const noexcept {
        return main_relfilenode_;
    }
    /// @return One-based sequence attribute number.
    [[nodiscard]] std::int16_t column_attno() const noexcept {
        return column_attno_;
    }
    /// @return Locator reference valid for this binding's lifetime.
    [[nodiscard]] const sequence_locator& locator() const noexcept {
        return locator_;
    }
    /// @return Unused letters in the final packed unit.
    [[nodiscard]] std::size_t last_fragment_padding() const noexcept {
        return trailing_padding_;
    }
    /// @return Borrowed registered MVCC snapshot.
    [[nodiscard]] Snapshot registered_snapshot() const noexcept {
        return registered_snapshot_;
    }
    /// @return Borrowed read-side shard cache.
    [[nodiscard]] seq_caches* caches() const noexcept {
        return caches_;
    }
    /// @return Parent context for callback-scoped session state.
    [[nodiscard]] MemoryContext parent_mctx() const noexcept {
        return parent_mctx_;
    }

    /// @return Registered alphabet code for `p_sequence_config`.
    [[nodiscard]] static constexpr std::uint8_t alphabet_code() noexcept {
        return static_cast<std::uint8_t>(
            pgseq_supported_alphabets::template index_of<alphabet_type>());
    }
    /// @return Packed storage-unit width in bytes.
    [[nodiscard]] static constexpr std::uint8_t storage_unit_byte_width() noexcept {
        return static_cast<std::uint8_t>(
            sizeof(typename compact_sequence_range<p_sequence_config>::traits::storage_unit));
    }
    /// @return Logical-letter capacity of one physical fragment.
    [[nodiscard]] static constexpr std::uint64_t fragment_bp_capacity() noexcept {
        return static_cast<std::uint64_t>(default_fragment_storage_units) *
               static_cast<std::uint64_t>(
                   compact_sequence_range<p_sequence_config>::traits::letters_per_unit);
    }
    /// Compute trailing padding for a locator.
    /// @param locator Locator whose logical length is measured in letters.
    /// @return Unused letters in the final packed unit.
    [[nodiscard]] static constexpr std::size_t
    expected_last_fragment_padding(const sequence_locator& locator) noexcept {
        constexpr std::uint64_t letters_per_unit =
            compact_sequence_range<p_sequence_config>::traits::letters_per_unit;
        const std::uint64_t remainder = locator.logical_length % letters_per_unit;
        return static_cast<std::size_t>(remainder == 0u ? 0u : letters_per_unit - remainder);
    }

    /// Compare every source, locator, and borrowed-lifetime identity field.
    /// @param other Binding to compare.
    /// @return Whether both bindings may reuse one concrete call state.
    [[nodiscard]] bool
    matches_same_source(const postgres_lazy_sequence_binding& other) const noexcept {
        return db_oid_ == other.db_oid_ && tablespace_oid_ == other.tablespace_oid_ &&
               main_relfilenode_ == other.main_relfilenode_ &&
               column_attno_ == other.column_attno_ &&
               std::memcmp(&locator_, &other.locator_, sizeof(locator_)) == 0 &&
               trailing_padding_ == other.trailing_padding_ &&
               registered_snapshot_ == other.registered_snapshot_ && caches_ == other.caches_ &&
               parent_mctx_ == other.parent_mctx_;
    }

  private:
    const Oid db_oid_;
    const Oid tablespace_oid_;
    const Oid main_relfilenode_;
    const std::int16_t column_attno_;
    const sequence_locator locator_;
    const std::size_t trailing_padding_;
    Snapshot const registered_snapshot_;
    seq_caches* const caches_;
    MemoryContext const parent_mctx_;
};

} // namespace bioseq::storage
