/**
 * @file sql_rows.cppm
 * @brief Catalog rows describing stored sequences and minimizer indexes.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

export module bioseq.postgres.storage.manager.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::catalog_rows {

/**
 * @brief Catalog entry for  one stored minimizr index generation.
 * The payload relation holds the index data. Provider 0 is the supported
 * storage type. State 1 means live and state 2 means retired.
 *
 * ```sql
 * -- As a catalog owner, inspect the live persisted minimizer generations.
 * SELECT user_relid::regclass AS relation, index_name, generation_id, image_bytes
 * FROM bioseq_internal.minimizer_generation_storage
 * WHERE state = 1
 * ORDER BY user_relid, column_attno, query_key;
 * ```
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .persistence = bioseq::sql::persistence::unlogged,
            .access_method = "heap",
        }),
        bioseq_sql::revoke({
            .privileges = bioseq::sql::privileges::all,
            .from = bioseq::sql::public_role,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "database_oid",
                "tablespace_oid",
                "main_relfilenode",
                "user_relid",
                "column_attno",
                "index_name",
                "query_key",
                "generation_id",
            },
        }),
        bioseq_sql::index({
            .name = "minimizer_generation_storage_one_live",
            .unique = true,
            .expression = bioseq::sql::checked_sql{
                R"SQL(database_oid, tablespace_oid, main_relfilenode,
user_relid, column_attno, index_name, query_key)SQL"},
            .predicate =
                bioseq::sql::checked_sql{"state = smallint '1'"},
        }),
        bioseq_sql::index({
            .name = "minimizer_generation_storage_retired",
            .expression = bioseq::sql::checked_sql{
                R"SQL(retire_full_xid, database_oid, tablespace_oid,
main_relfilenode, user_relid, column_attno, index_name, query_key,
generation_id)SQL"},
            .predicate =
                bioseq::sql::checked_sql{"state = smallint '2'"},
        }),
        bioseq_sql::config_dump]] minimizer_generation_storage {
    /// @brief OID of the database containing the indexed user table.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"database_oid <> 0"},
    })]]
    Oid database_oid;

    /// @brief OID of the tablespace containing the indexed user table.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"tablespace_oid <> 0"},
    })]]
    Oid tablespace_oid;

    /// @brief Physical file number of the indexed user table.
    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{"main_relfilenode <> 0"},
    })]]
    Oid main_relfilenode;

    /// @brief OID of the indexed user table.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"user_relid <> 0"},
    })]]
    Oid user_relid;

    /// @brief One-based attribute number of the indexed sequence column.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"column_attno > 0"},
    })]]
    std::int16_t column_attno;

    /// @brief Index format name; currently minimizers_per_fragment.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            "index_name = 'minimizers_per_fragment'"},
    })]]
    NameData index_name;

    /// @brief Sequence row number identifying the indexed source within its shard.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"query_key >= 0"},
    })]]
    std::int64_t query_key;

    /// @brief Positive identifier allocated for this index generation.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"generation_id > 0"},
    })]]
    std::int64_t generation_id;

    /// @brief Storage provider identifier; currently only 0 is supported.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"provider = 0"},
    })]]
    std::int16_t provider;

    /// @brief OID of the relation holding this generation payload.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            R"SQL(payload_relation_oid <> 0
AND payload_database_oid = database_oid
AND payload_tablespace_oid <> 0
AND payload_relfilenode <> 0)SQL"},
    })]]
    Oid payload_relation_oid;

    /// @brief Database OID of the payload; must match database_oid.
    Oid payload_database_oid;
    /// @brief Tablespace OID of the payload relation.
    Oid payload_tablespace_oid;
    /// @brief Physical file number of the payload relation.
    Oid payload_relfilenode;
    /// @brief Name recorded for the payload relation.
    NameData payload_relation_name;

    /// @brief Index image size in bytes, including its header; at least 4096.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"image_bytes >= 4096"},
    })]]
    std::int64_t image_bytes;

    /// @brief Checksum recorded for the index image header.
    std::int64_t header_checksum;

    /// @brief Full transaction ID that retired the generation; NULL while it is live.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            R"SQL((state = 1 AND retire_full_xid IS NULL)
OR (state = 2 AND retire_full_xid IS NOT NULL))SQL"},
    })]]
    std::optional<FullTransactionId> retire_full_xid;

    /// @brief Generation state: 1 means live and 2 means retired.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"state IN (1, 2)"},
    })]]
    std::int16_t state;
};

/** Saved directory position for cleaning up orphaned index image files.
 *
 * ```sql
 * -- As a catalog owner, inspect where orphaned index-image cleanup will resume.
 * SELECT database_oid, tablespace_root_ordinal, shard_ordinal,
 *        last_canonical_filename, pass
 * FROM bioseq_internal.minimizer_generation_reaper_state
 * ORDER BY database_oid;
 * ```
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .persistence = bioseq::sql::persistence::unlogged,
            .access_method = "heap",
        }),
        bioseq_sql::revoke({
            .privileges = bioseq::sql::privileges::all,
            .from = bioseq::sql::public_role,
        }),
        bioseq_sql::primary_key({
            .placement =
                bioseq::sql::constraint_placement::column_inline,
            .columns = {"database_oid"},
        }),
        bioseq_sql::config_dump]] minimizer_generation_reaper_state {
    /// @brief Database whose orphaned index-image scan is being tracked.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"database_oid <> 0"},
    })]]
    Oid database_oid;

    /// @brief Saved tablespace-root position of the cleanup scan.
    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{"tablespace_root_ordinal >= 0"},
    })]]
    std::int32_t tablespace_root_ordinal = 0;

    /// @brief Saved shard-directory position, from 0 through 255.
    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{
                "shard_ordinal BETWEEN 0 AND 255"},
    })]]
    std::int16_t shard_ordinal = 0;

    /// @brief Last canonical file name reached by the cleanup scan.
    std::string last_canonical_filename = std::string{};

    /// @brief Nonnegative cleanup pass counter.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"pass >= 0"},
    })]]
    std::int64_t pass = 0;
};

/**
 * @brief Version and checksum for a stored sequence and its fragment range.
 * PostgreSQL row visibility determines which version a reader can use. The
 * checksum is computed when the sequence is inserted State 0 means the
 * sequence is awaiting publication, 1 means ready, and 2 means invalid.
 *
 * ```sql
 * -- As a catalog owner, inspect stored sequence versions and publication states.
 * SELECT user_relid::regclass AS relation, source_epoch,
 *        num_fragments, total_bp_size, state
 * FROM bioseq_internal.sequence_source_version
 * ORDER BY source_epoch;
 * ```
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .access_method = "heap",
        }),
        bioseq_sql::revoke({
            .privileges = bioseq::sql::privileges::all,
            .from = bioseq::sql::public_role,
        }),
        bioseq_sql::primary_key({
            .name = "sequence_source_version_pkey",
            .columns = {"source_epoch"},
        }),
        bioseq_sql::index({
            .name = "sequence_source_version_locator_key",
            .unique = true,
            .expression = bioseq::sql::checked_sql{
                R"SQL(fragment_relid, fragment_storage_id,
first_sub_row_no)SQL"},
        }),
        bioseq_sql::index({
            .name = "sequence_source_version_fragment_row_idx",
            .expression = bioseq::sql::checked_sql{
                "fragment_relid, first_sub_row_no"},
        }),
        bioseq_sql::config_dump]] sequence_source_version {
    /// @brief Positive immutable version identifier allocated for the stored sequence.
    [[bioseq_sql::check({
        .name = "sequence_source_version_source_epoch_check",
        .expression = bioseq::sql::checked_sql{"source_epoch > 0"},
    })]]
    std::int64_t source_epoch;

    /// @brief OID of the parent user table.
    Oid user_relid;
    /// @brief OID of the parent database.
    Oid database_oid;
    /// @brief OID of the parent tablespace.
    Oid tablespace_oid;
    /// @brief Physical file number of the parent table.
    Oid main_relfilenode;

    /// @brief Storage generation of the parent table.
    [[bioseq_sql::check({
        .name = "sequence_source_version_main_storage_id_check",
        .expression = bioseq::sql::checked_sql{"main_storage_id > 0"},
    })]]
    std::int64_t main_storage_id;

    /// @brief One-based attribute number of the parent sequence column.
    [[bioseq_sql::check({
        .name = "sequence_source_version_column_attno_check",
        .expression = bioseq::sql::checked_sql{"column_attno > 0"},
    })]]
    std::int16_t column_attno;

    /// @brief Sequence-locator format version; currently 4.
    [[bioseq_sql::check({
        .name = "sequence_source_version_locator_version_check",
        .expression = bioseq::sql::checked_sql{"locator_version = 4"},
    })]]
    std::int16_t locator_version;

    /// @brief Length-bucket index recorded in the sequence locator.
    [[bioseq_sql::check({
        .name = "sequence_source_version_bucket_idx_check",
        .expression =
            bioseq::sql::checked_sql{"bucket_idx BETWEEN 0 AND 255"},
    })]]
    std::int16_t bucket_idx;

    /// @brief Shard number within the length bucket.
    [[bioseq_sql::check({
        .name = "sequence_source_version_shard_idx_check",
        .expression = bioseq::sql::checked_sql{"shard_idx >= 0"},
    })]]
    std::int32_t shard_idx;

    /// @brief OID of the fragment table; NULL for an empty sequence.
    [[bioseq_sql::check({
        .name = "sequence_source_version_fragment_shape_check",
        .expression = bioseq::sql::checked_sql{
            R"SQL((num_fragments = 0
AND fragment_relid IS NULL
AND fragment_storage_id = 0
AND first_sub_row_no = 0)
OR (num_fragments > 0
AND fragment_relid IS NOT NULL
AND fragment_storage_id > 0))SQL"},
    })]]
    std::optional<Oid> fragment_relid;

    /// @brief Storage generation of the fragment table; 0 when there are no fragments.
    [[bioseq_sql::check({
        .name =
            "sequence_source_version_fragment_storage_id_check",
        .expression =
            bioseq::sql::checked_sql{"fragment_storage_id >= 0"},
    })]]
    std::int64_t fragment_storage_id;

    /// @brief Logical row number of the first fragment; 0 when there are no fragments.
    [[bioseq_sql::check({
        .name = "sequence_source_version_first_sub_row_no_check",
        .expression =
            bioseq::sql::checked_sql{"first_sub_row_no >= 0"},
    })]]
    std::int64_t first_sub_row_no;

    /// @brief Number of consecutive fragment rows belonging to the sequence.
    [[bioseq_sql::check({
        .name = "sequence_source_version_num_fragments_check",
        .expression = bioseq::sql::checked_sql{
            "num_fragments BETWEEN 0 AND 4294967295"},
    })]]
    std::int64_t num_fragments;

    /// @brief Sequence length in decoded symbols.
    [[bioseq_sql::check({
        .name = "sequence_source_version_total_bp_size_check",
        .expression =
            bioseq::sql::checked_sql{"total_bp_size >= 0"},
    })]]
    std::int64_t total_bp_size;

    /// @brief Numeric code identifying the sequence alphabet.
    [[bioseq_sql::check({
        .name = "sequence_source_version_alphabet_code_check",
        .expression = bioseq::sql::checked_sql{
            "alphabet_code BETWEEN 0 AND 255"},
    })]]
    std::int16_t alphabet_code;

    /// @brief Width of one packed storage unit in bytes, from 1 through 15.
    [[bioseq_sql::check({
        .name =
            "sequence_source_version_storage_unit_byte_width_check",
        .expression = bioseq::sql::checked_sql{
            "storage_unit_byte_width BETWEEN 1 AND 15"},
    })]]
    std::int16_t storage_unit_byte_width;

    /// @brief Number of unused letter slots in the final packed storage unit.
    [[bioseq_sql::check({
        .name = "sequence_source_version_padding_letters_check",
        .expression = bioseq::sql::checked_sql{
            "padding_letters BETWEEN 0 AND 65535"},
    })]]
    std::int32_t padding_letters;

    /// @brief Physical codec: ordinary packed ranks or canonical run-length bytes.
    [[bioseq_sql::check({
        .name = "sequence_source_version_encoding_check",
        .expression = bioseq::sql::checked_sql{"encoding BETWEEN 0 AND 1"},
    })]]
    std::int16_t encoding;

    /// @brief Projection-space ordinal; validated against the alphabet at read time.
    [[bioseq_sql::check({
        .name = "sequence_source_version_cigar_space_check",
        .expression = bioseq::sql::checked_sql{"cigar_space BETWEEN 0 AND 255"},
    })]]
    std::int16_t cigar_space;

    /// @brief Canonical RLE byte count; zero for packed ranks and empty sources.
    [[bioseq_sql::check({
        .name = "sequence_source_version_encoded_payload_bytes_check",
        .expression = bioseq::sql::checked_sql{
            "encoded_payload_bytes BETWEEN 0 AND 4294967295"},
    })]]
    std::int64_t encoded_payload_bytes;

    /// @brief CRC-64 checksum of canonical symbols or canonical RLE bytes.
    std::int64_t source_content_checksum;

    /// @brief Publication state: 0 is unpublished, 1 is ready, and 2 is invalid.
    [[bioseq_sql::check({
        .name = "sequence_source_version_state_check",
        .expression = bioseq::sql::checked_sql{"state IN (0, 1, 2)"},
    })]]
    std::int16_t state;
};

static_assert(std::is_aggregate_v<minimizer_generation_storage>);
static_assert(std::is_aggregate_v<minimizer_generation_reaper_state>);
static_assert(std::is_aggregate_v<sequence_source_version>);

}  // namespace bioseq::storage::catalog_rows

export namespace bioseq::storage {

/** Result of one orphaned sequence-storage cleanup attempt. */
struct reaper_event {
    std::string action;
    std::string target;
};

static_assert(std::is_aggregate_v<reaper_event>);

}  // namespace bioseq::storage
