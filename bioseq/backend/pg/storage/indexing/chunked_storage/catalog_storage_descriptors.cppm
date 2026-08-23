/**
 * @file catalog_storage_descriptors.cppm
 * @brief Defines generation storage descriptors and publication capabilities.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "storage/relfilelocator.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/xid8.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <sys/types.h>

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:catalog_storage_descriptors;

import std;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI

import :catalog_contracts;
import :catalog_metadata;
import :catalog_source_authorization;
export namespace bioseq::storage::indexing {
class index_storage_fragment_generation_row_sink;
class ready_minimizer_generation_token;

/// Exact receipt returned by the immutable minimizer fragment image writer. Device/inode
/// are transaction-local sealing receipts. The descriptor persists the byte
/// length and authenticated header checksum.
struct minimizer_fragment_image_file_receipt {
    std::uint64_t file_bytes = 0u;
    std::uint64_t payload_crc64 = 0u;
    std::uint64_t header_crc64 = 0u;
    dev_t device{};
    ino_t inode{};
};

/// Publication lifecycle stored in the generation descriptor catalog.
enum class minimizer_generation_storage_state : std::int16_t {
    live = 1,
    retired = 2,
};

/// Production row model for
/// bioseq_internal.minimizer_generation_storage. The SQL catalog requires the
/// complete relation locator. Defensive in-memory optionals prevent a malformed
/// raw row from acquiring relation authority through a default value.
struct minimizer_generation_storage_descriptor {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name{};
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    std::optional<Oid> payload_relation_oid{};
    std::optional<Oid> payload_database_oid{};
    std::optional<Oid> payload_tablespace_oid{};
    std::optional<Oid> payload_relfilenode{};
    std::optional<std::string> payload_relation_name{};
    std::uint64_t image_bytes = 0u;
    std::uint64_t header_checksum = 0u;
    std::optional<std::uint64_t> retire_full_xid{};
    minimizer_generation_storage_state state = minimizer_generation_storage_state::live;
};

/// Field count of the generated descriptor-row codec.
inline constexpr std::size_t k_storage_descriptor_attribute_count =
    minimizer_generation_storage_codec::field_count;

/// Report whether a descriptor belongs to the requested catalog identity.
[[nodiscard]] inline bool
exact_storage_descriptor_identity(const minimizer_generation_storage_descriptor& descriptor,
                                  const minimizer_generation_administration_identity& identity,
                                  const std::uint64_t query_key) noexcept {
    return descriptor.database_oid == identity.database_oid &&
           descriptor.tablespace_oid == identity.tablespace_oid &&
           descriptor.main_relfilenode == identity.main_relfilenode &&
           descriptor.user_relid == identity.user_relid &&
           descriptor.column_attno == identity.column_attno &&
           descriptor.index_name == identity.index_name && descriptor.query_key == query_key;
}

/// Report whether every relation-locator field carries valid authority.
[[nodiscard]] inline bool has_exact_storage_descriptor_relation_shape(
    const minimizer_generation_storage_descriptor& descriptor) noexcept {
    return descriptor.payload_relation_oid && OidIsValid(*descriptor.payload_relation_oid) &&
           descriptor.payload_database_oid &&
           *descriptor.payload_database_oid == descriptor.database_oid &&
           descriptor.payload_tablespace_oid && OidIsValid(*descriptor.payload_tablespace_oid) &&
           descriptor.payload_relfilenode && OidIsValid(*descriptor.payload_relfilenode) &&
           descriptor.payload_relation_name && !descriptor.payload_relation_name->empty();
}

/// Immutable receipt for one atomically published grouped-SoA head.
struct ready_minimizer_generation_published_head_receipt {
    std::string table_name;
    std::uint64_t query_key = 0;
    minimizer_generation_head head{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes> head_bytes{};
    std::optional<std::uint64_t> replaced_generation_id{};
};

template <class p_receipt>
concept reports_ready_minimizer_generation_acceptance =
    requires(const p_receipt& receipt) { receipt.published(); } ||
    requires(const p_receipt& receipt) { receipt.execution_accepted(); } ||
    requires(const p_receipt& receipt) { receipt.accepted(); };

static_assert(!reports_ready_minimizer_generation_acceptance<
              ready_minimizer_generation_published_head_receipt>);

/// Consume a validated token and atomically replace the active head.
[[nodiscard]] ready_minimizer_generation_published_head_receipt
publish_ready_minimizer_generation_head_pg(ready_minimizer_generation_token&& token);

/// Move-only authorization that one exact relation/source/generation was completely
/// reread under an own-write-visible registered snapshot. Construction is
/// private to the generation sink's full validation pass.
class ready_minimizer_generation_token {
  private:
    minimizer_generation_administration_identity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t source_identity_ = 0;
    std::uint64_t source_content_checksum_ = 0;
    std::uint64_t storage_contract_checksum_ = 0;
    TransactionId transaction_id_ = InvalidTransactionId;
    std::array<std::byte, minimizer_generation_head::encoded_bytes> head_bytes_{};
    std::uint64_t head_row_checksum_ = 0;
    std::optional<minimizer_fragment_image_file_receipt> file_receipt_{};
    bool valid_ = false;

    ready_minimizer_generation_token(
        minimizer_generation_administration_identity identity,
        Oid sidecar_relation_oid,
        RelFileLocator sidecar_locator,
        std::uint64_t advisory_lock_key,
        std::uint64_t query_key,
        std::uint64_t generation_id,
        std::uint64_t source_identity,
        std::uint64_t source_content_checksum,
        std::uint64_t storage_contract_checksum,
        TransactionId transaction_id,
        std::span<const std::byte> head_bytes,
        std::uint64_t head_row_checksum,
        std::optional<minimizer_fragment_image_file_receipt> file_receipt)
        : identity_(std::move(identity)), sidecar_relation_oid_(sidecar_relation_oid),
          sidecar_locator_(sidecar_locator), advisory_lock_key_(advisory_lock_key),
          query_key_(query_key), generation_id_(generation_id), source_identity_(source_identity),
          source_content_checksum_(source_content_checksum),
          storage_contract_checksum_(storage_contract_checksum), transaction_id_(transaction_id),
          head_row_checksum_(head_row_checksum), file_receipt_(std::move(file_receipt)),
          valid_(true) {
        std::copy(head_bytes.begin(), head_bytes.end(), head_bytes_.begin());
    }

    void invalidate() noexcept {
        identity_.release_owned_strings();
        sidecar_relation_oid_ = InvalidOid;
        sidecar_locator_ = {};
        advisory_lock_key_ = 0;
        query_key_ = 0;
        generation_id_ = 0;
        source_identity_ = 0;
        source_content_checksum_ = 0;
        storage_contract_checksum_ = 0;
        transaction_id_ = InvalidTransactionId;
        head_bytes_.fill(std::byte{0});
        head_row_checksum_ = 0;
        file_receipt_.reset();
        valid_ = false;
    }

    friend class index_storage_fragment_generation_row_sink;
    friend ready_minimizer_generation_published_head_receipt
    publish_ready_minimizer_generation_head_pg(ready_minimizer_generation_token&& token);

  public:
    ready_minimizer_generation_token() = delete;
    ready_minimizer_generation_token(const ready_minimizer_generation_token&) = delete;
    ready_minimizer_generation_token& operator=(const ready_minimizer_generation_token&) = delete;
    ready_minimizer_generation_token(ready_minimizer_generation_token&& other) noexcept
        : identity_(std::move(other.identity_)), sidecar_relation_oid_(other.sidecar_relation_oid_),
          sidecar_locator_(other.sidecar_locator_), advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_), generation_id_(other.generation_id_),
          source_identity_(other.source_identity_),
          source_content_checksum_(other.source_content_checksum_),
          storage_contract_checksum_(other.storage_contract_checksum_),
          transaction_id_(other.transaction_id_), head_bytes_(other.head_bytes_),
          head_row_checksum_(other.head_row_checksum_),
          file_receipt_(std::move(other.file_receipt_)), valid_(other.valid_) {
        other.invalidate();
    }
    ready_minimizer_generation_token& operator=(ready_minimizer_generation_token&& other) noexcept {
        if (this != &other) {
            invalidate();
            identity_ = std::move(other.identity_);
            sidecar_relation_oid_ = other.sidecar_relation_oid_;
            sidecar_locator_ = other.sidecar_locator_;
            advisory_lock_key_ = other.advisory_lock_key_;
            query_key_ = other.query_key_;
            generation_id_ = other.generation_id_;
            source_identity_ = other.source_identity_;
            source_content_checksum_ = other.source_content_checksum_;
            storage_contract_checksum_ = other.storage_contract_checksum_;
            transaction_id_ = other.transaction_id_;
            head_bytes_ = other.head_bytes_;
            head_row_checksum_ = other.head_row_checksum_;
            file_receipt_ = std::move(other.file_receipt_);
            valid_ = other.valid_;
            other.invalidate();
        }
        return *this;
    }
    ~ready_minimizer_generation_token() = default;
};

static_assert(!std::is_default_constructible_v<ready_minimizer_generation_token>);
static_assert(!std::is_copy_constructible_v<ready_minimizer_generation_token>);
static_assert(!std::is_copy_assignable_v<ready_minimizer_generation_token>);
static_assert(std::is_nothrow_move_constructible_v<ready_minimizer_generation_token>);
static_assert(std::is_nothrow_move_assignable_v<ready_minimizer_generation_token>);

/// Read the sole LIVE descriptor for one exact storage identity through the
/// catalog's partial unique index.  The caller owns both relation locks and
/// the registered MVCC snapshot. This helper neither substitutes SnapshotSelf
/// nor consults mutable payload-routing state.
[[nodiscard]] inline minimizer_generation_storage_descriptor
read_live_minimizer_generation_storage_descriptor_pg(
    Relation storage_relation,
    Relation live_index_relation,
    Snapshot registered_snapshot,
    const minimizer_generation_administration_identity& identity,
    const std::uint64_t query_key,
    const std::uint64_t expected_generation_id) {
    using namespace bioseq::indexing;
    if (storage_relation == nullptr || live_index_relation == nullptr ||
        registered_snapshot == nullptr || !IsMVCCSnapshot(registered_snapshot) ||
        registered_snapshot->regd_count == 0 || live_index_relation->rd_index == nullptr ||
        live_index_relation->rd_index->indrelid != RelationGetRelid(storage_relation) ||
        std::string_view{RelationGetRelationName(live_index_relation)} !=
            "minimizer_generation_storage_one_live" ||
        !OidIsValid(identity.database_oid) || !OidIsValid(identity.tablespace_oid) ||
        !OidIsValid(identity.main_relfilenode) || !OidIsValid(identity.user_relid) ||
        identity.column_attno <= 0 || identity.index_name != k_minimizers_per_fragment_index_name ||
        !sql_bigint_can_represent(query_key) || expected_generation_id == 0u ||
        !sql_bigint_can_represent(expected_generation_id)) {
        minimizer_generation_storage_error_as_cpp(
            "LIVE generation descriptor lookup binding is invalid");
    }

    NameData index_name{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&index_name, &identity] { namestrcpy(&index_name, identity.index_name.c_str()); });
    std::array<ScanKeyData, 7> keys{
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::database_oid>(identity.database_oid),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::tablespace_oid>(identity.tablespace_oid),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::main_relfilenode>(identity.main_relfilenode),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::user_relid>(identity.user_relid),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::column_attno>(identity.column_attno),
        {},
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::query_key>(
            static_cast<std::int64_t>(query_key)),
    };
    ScanKeyInit(
        &keys[5],
        minimizer_generation_storage_codec::attribute_number<
            &minimizer_generation_storage_catalog_row::index_name>(),
        BTEqualStrategyNumber,
        minimizer_generation_storage_codec::
            field<&minimizer_generation_storage_catalog_row::index_name>::comparison_procedure(
                bioseq::postgres::support::scan_comparison::equal),
        NameGetDatum(&index_name));

    SysScanDesc scan = nullptr;
    try {
        scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [storage_relation, live_index_relation, registered_snapshot, &keys] {
                return systable_beginscan_ordered(storage_relation,
                                                  live_index_relation,
                                                  registered_snapshot,
                                                  static_cast<int>(keys.size()),
                                                  keys.data());
            });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
        if (!HeapTupleIsValid(tuple)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation storage descriptor is missing");
        }

        std::optional<minimizer_generation_storage_catalog_row> decoded_catalog;
        try {
            decoded_catalog.emplace(minimizer_generation_storage_codec::deform(
                tuple, RelationGetDescr(storage_relation), CurrentMemoryContext));
        } catch (const bioseq::postgres::support::row_codec_error& error) {
            minimizer_generation_storage_error_as_cpp(error.what());
        }
        const minimizer_generation_storage_catalog_row& catalog = *decoded_catalog;

        const std::int64_t query_key_sql = catalog.query_key;
        const std::int64_t generation_id_sql = catalog.generation_id;
        const std::int16_t storage_kind_sql = catalog.provider;
        const std::int64_t image_bytes_sql = catalog.image_bytes;
        const std::int16_t state_sql = catalog.state;
        if (query_key_sql < 0 || generation_id_sql <= 0 ||
            image_bytes_sql < static_cast<std::int64_t>(k_minimizer_fragment_image_header_bytes) ||
            storage_kind_sql !=
                static_cast<std::int16_t>(
                    bioseq::indexing::k_minimizer_fragment_image_relation_backed_storage_kind)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor scalar shape is invalid");
        }

        minimizer_generation_storage_descriptor descriptor{
            .database_oid = catalog.database_oid,
            .tablespace_oid = catalog.tablespace_oid,
            .main_relfilenode = catalog.main_relfilenode,
            .user_relid = catalog.user_relid,
            .column_attno = catalog.column_attno,
            .index_name = NameStr(catalog.index_name),
            .query_key = static_cast<std::uint64_t>(query_key_sql),
            .generation_id = static_cast<std::uint64_t>(generation_id_sql),
            .payload_relation_oid = std::optional<Oid>{catalog.payload_relation_oid},
            .payload_database_oid = std::optional<Oid>{catalog.payload_database_oid},
            .payload_tablespace_oid = std::optional<Oid>{catalog.payload_tablespace_oid},
            .payload_relfilenode = std::optional<Oid>{catalog.payload_relfilenode},
            .payload_relation_name =
                std::optional<std::string>{NameStr(catalog.payload_relation_name)},
            .image_bytes = static_cast<std::uint64_t>(image_bytes_sql),
            .header_checksum = checksum_from_pg_bigint(catalog.header_checksum),
            .retire_full_xid = catalog.retire_full_xid
                                   ? std::optional<std::uint64_t>{U64FromFullTransactionId(
                                         *catalog.retire_full_xid)}
                                   : std::nullopt,
            .state = static_cast<minimizer_generation_storage_state>(state_sql),
        };
        if (!exact_storage_descriptor_identity(descriptor, identity, query_key) ||
            descriptor.generation_id != expected_generation_id ||
            descriptor.state != minimizer_generation_storage_state::live ||
            descriptor.retire_full_xid ||
            !has_exact_storage_descriptor_relation_shape(descriptor)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor disagrees with the active head");
        }

        HeapTuple duplicate = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
        if (HeapTupleIsValid(duplicate)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor lookup returned duplicates");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan_ordered(scan); });
        scan = nullptr;
        return descriptor;
    } catch (const bioseq::postgres::support::postgres_error&) {
        scan = nullptr;
        throw;
    } catch (...) {
        if (scan != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan_ordered(scan); });
            scan = nullptr;
        }
        throw;
    }
}

} // namespace bioseq::storage::indexing
