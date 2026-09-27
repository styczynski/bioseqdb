/**
 * @file executor_read_context.cppm
 * @brief Borrow an executor relation and snapshot for index reads.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <optional>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.indexing.executor_read_context;

export namespace bioseq::storage::indexing {

struct borrowed_executor_read_context_access;

/**
 * @brief Borrow a locked relation and a registered MVCC snapshot.
 *
 * The caller must keep both resources alive while this context is used. Moving
 * the context transfers its references. Destruction leaves PostgreSQL resources
 * with their owner.
 */
class borrowed_executor_read_context {
  private:
    friend struct borrowed_executor_read_context_access;

    Relation relation_ = nullptr;
    Snapshot snapshot_ = nullptr;
    Oid relation_oid_ = InvalidOid;
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid relfilenode_ = InvalidOid;

    void invalidate_() noexcept {
        relation_ = nullptr;
        snapshot_ = nullptr;
        relation_oid_ = InvalidOid;
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        relfilenode_ = InvalidOid;
    }

    borrowed_executor_read_context(Relation relation, Snapshot registered_snapshot) noexcept
        : relation_(relation), snapshot_(registered_snapshot),
          relation_oid_(relation == nullptr ? InvalidOid : RelationGetRelid(relation)),
          database_oid_(relation == nullptr ? InvalidOid : relation->rd_locator.dbOid),
          tablespace_oid_(relation == nullptr ? InvalidOid : relation->rd_locator.spcOid),
          relfilenode_(relation == nullptr ? InvalidOid
                                           : static_cast<Oid>(relation->rd_locator.relNumber)) {}

  public:
    borrowed_executor_read_context() = delete;

    /**
     * @brief Bind a relation to its registered MVCC snapshot.
     * @return A context if the snapshot and relation identity are valid, otherwise no value.
     */
    [[nodiscard]] static std::optional<borrowed_executor_read_context>
    bind_checked(Relation relation, Snapshot registered_snapshot) noexcept {
        borrowed_executor_read_context context{relation, registered_snapshot};
        if (!context.valid())
            return std::nullopt;
        return std::optional<borrowed_executor_read_context>{std::move(context)};
    }

    borrowed_executor_read_context(const borrowed_executor_read_context&) = delete;
    borrowed_executor_read_context& operator=(const borrowed_executor_read_context&) = delete;
    borrowed_executor_read_context(borrowed_executor_read_context&& other) noexcept
        : relation_(std::exchange(other.relation_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          relation_oid_(std::exchange(other.relation_oid_, InvalidOid)),
          database_oid_(std::exchange(other.database_oid_, InvalidOid)),
          tablespace_oid_(std::exchange(other.tablespace_oid_, InvalidOid)),
          relfilenode_(std::exchange(other.relfilenode_, InvalidOid)) {}
    borrowed_executor_read_context& operator=(borrowed_executor_read_context&& other) noexcept {
        if (this != &other) {
            invalidate_();
            relation_ = std::exchange(other.relation_, nullptr);
            snapshot_ = std::exchange(other.snapshot_, nullptr);
            relation_oid_ = std::exchange(other.relation_oid_, InvalidOid);
            database_oid_ = std::exchange(other.database_oid_, InvalidOid);
            tablespace_oid_ = std::exchange(other.tablespace_oid_, InvalidOid);
            relfilenode_ = std::exchange(other.relfilenode_, InvalidOid);
        }
        return *this;
    }
    ~borrowed_executor_read_context() = default;

    /// @return True if the snapshot is registered and the relation identity still matches.
    [[nodiscard]] bool valid() const noexcept {
        return relation_ != nullptr && snapshot_ != nullptr && IsMVCCSnapshot(snapshot_) &&
               snapshot_->regd_count > 0 && OidIsValid(relation_oid_) &&
               OidIsValid(database_oid_) && OidIsValid(tablespace_oid_) &&
               OidIsValid(relfilenode_) && RelationGetRelid(relation_) == relation_oid_ &&
               relation_->rd_locator.dbOid == database_oid_ &&
               relation_->rd_locator.spcOid == tablespace_oid_ &&
               static_cast<Oid>(relation_->rd_locator.relNumber) == relfilenode_;
    }

    /// @return True if this valid context refers to the supplied relation identity.
    [[nodiscard]] bool matches(Oid database_oid,
                               Oid tablespace_oid,
                               Oid relfilenode,
                               Oid relation_oid) const noexcept {
        return valid() && database_oid_ == database_oid && tablespace_oid_ == tablespace_oid &&
               relfilenode_ == relfilenode && relation_oid_ == relation_oid;
    }

    /// @return Borrowed locked relation.
    [[nodiscard]] Relation relation() const noexcept {
        return relation_;
    }
    /// @return Borrowed registered snapshot.
    [[nodiscard]] Snapshot snapshot() const noexcept {
        return snapshot_;
    }
    /// @return Relation OID captured at binding.
    [[nodiscard]] Oid relation_oid() const noexcept {
        return relation_oid_;
    }
    /// @return Database OID captured at binding.
    [[nodiscard]] Oid database_oid() const noexcept {
        return database_oid_;
    }
    /// @return Tablespace OID captured at binding.
    [[nodiscard]] Oid tablespace_oid() const noexcept {
        return tablespace_oid_;
    }
    /// @return Relation file number captured at binding.
    [[nodiscard]] Oid relfilenode() const noexcept {
        return relfilenode_;
    }
};

/**
 * @brief Let administrative reads use the same source checks as executor reads.
 *
 * This helper creates an unchecked context. Pass it directly to catalog source
 * authorization, which checks the relation identity and registered snapshot.
 */
struct borrowed_executor_read_context_access {
    /// @return An unchecked context for catalog source authorization to validate.
    [[nodiscard]] static borrowed_executor_read_context
    bind_for_catalog_authorization(Relation relation, Snapshot registered_snapshot) noexcept {
        return borrowed_executor_read_context{relation, registered_snapshot};
    }
};

static_assert(!std::is_copy_constructible_v<borrowed_executor_read_context>);
static_assert(std::is_nothrow_move_constructible_v<borrowed_executor_read_context>);

} // namespace bioseq::storage::indexing
