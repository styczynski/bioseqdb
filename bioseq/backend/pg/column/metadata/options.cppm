/**
 * @file options.cppm
 * @brief Column-table option operations exposed by the metadata module.
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "safe_lib.h"

export module bioseq.postgres.storage.metadata:options;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import :types;
import :tuple_ops;
import :core;

/**
 * @brief Converts a generated catalog row into the runtime option model.
 * @param[in] row Decoded non-null catalog fields.
 * @return Runtime options with compression text resolved to its enum.
 */
[[nodiscard]] static column_options options_to_domain(const options_row& row) {
    return column_options{
        .stripe_row_limit = static_cast<uint64>(row.stripe_row_limit),
        .chunk_group_row_limit = static_cast<uint32>(row.chunk_group_row_limit),
        .compression_algorithm = parse_compression_algorithm(NameStr(row.compression)),
        .compression_level = row.compression_level,
    };
}

/**
 * @brief Reads stored relation options or derives defaults when no row exists.
 * @param[in] regclass Relation OID used as the catalog key.
 * @return Options, or no value when extension removal has made the catalog unavailable.
 * @note PostgreSQL resources close before any captured C++ exception is rethrown.
 */
export std::optional<column_options> read_column_options(Oid regclass) {
    ScanKeyData scan_keys[1];
    scan_keys[0] = options_codec::scan_key<&options_row::regclass>(regclass);

    Relation bioseq_column_options = bioseq::postgres::support::invoke_pg_as_cpp([] {
        return bioseq::postgres::support::try_open_postgres_relation(column_options_relation_oid(),
                                                                     AccessShareLock);
    });
    if (bioseq_column_options == nullptr) {
        /* Extension has been dropped (e.g. during DROP EXTENSION/DATABASE). */
        return std::nullopt;
    }

    Relation index = nullptr;
    SysScanDesc scan_descriptor = nullptr;
    std::optional<column_options> result;
    std::exception_ptr failure;
    try {
        index = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return bioseq::postgres::support::try_open_postgres_relation(column_options_index_oid(),
                                                                         AccessShareLock);
        });
        if (index != nullptr) {
            scan_descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
                [bioseq_column_options, index, &scan_keys] {
                    return systable_beginscan_ordered(
                        bioseq_column_options, index, nullptr, 1, scan_keys);
                });
            HeapTuple heap_tuple = bioseq::postgres::support::invoke_pg_as_cpp([scan_descriptor] {
                return systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
            });
            if (HeapTupleIsValid(heap_tuple)) {
                const options_row row = options_codec::deform(
                    heap_tuple, RelationGetDescr(bioseq_column_options), CurrentMemoryContext);
                result = options_to_domain(row);
            } else {
                /* Populate options with system defaults. */
                result = column_options{
                    .stripe_row_limit =
                        static_cast<uint32_t>(bioseq::settings::storage::stripe::row_limit),
                    .chunk_group_row_limit =
                        static_cast<uint32_t>(bioseq::settings::storage::chunk_group::row_limit),
                    .compression_algorithm =
                        static_cast<bioseq::compression_algorithm>(static_cast<int>(
                            bioseq::settings::storage::compression::algorithm.get_value())),
                    .compression_level = bioseq::settings::storage::compression::level,
                };
            }
        }
    } catch (...) {
        failure = std::current_exception();
    }

    if (scan_descriptor != nullptr) {
        record_cleanup_failure(failure, [scan_descriptor] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [scan_descriptor] { systable_endscan_ordered(scan_descriptor); });
        });
    }
    if (index != nullptr) {
        record_cleanup_failure(failure, [index] {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [index] { index_close(index, AccessShareLock); });
        });
    }
    record_cleanup_failure(failure, [bioseq_column_options] {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [bioseq_column_options] { relation_close(bioseq_column_options, AccessShareLock); });
    });
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return result;
}

/**
 * @brief Inserts configured defaults for a relation that has no options row.
 * @param[in] regclass Relation OID used as the catalog key.
 * @note Binary-upgrade restore owns catalog population and makes this a no-op.
 */
export void initialize_column_options(Oid regclass) {
    /*
     * When upgrading we retain options for all columnar tables by upgrading
     * "bioseq.options" catalog table, so we shouldn't do anything here.
     */
    if (IsBinaryUpgrade) {
        return;
    }

    column_options default_options = {
        .stripe_row_limit = static_cast<uint32_t>(bioseq::settings::storage::stripe::row_limit),
        .chunk_group_row_limit =
            static_cast<uint32_t>(bioseq::settings::storage::chunk_group::row_limit),
        .compression_algorithm = static_cast<bioseq::compression_algorithm>(
            static_cast<int>(bioseq::settings::storage::compression::algorithm.get_value())),
        .compression_level = bioseq::settings::storage::compression::level};

    write_column_options(regclass, &default_options, false);
}

/// @brief Replaces the authoritative column options for a relation.
/// @param[in] regclass Relation OID used as the catalog key.
/// @param[in] options Borrowed option values copied into the catalog row.
export void set_column_options(Oid regclass, column_options* options) {
    write_column_options(regclass, options, true);
}

/**
 * @brief Writes relation options subject to an overwrite policy.
 * @param[in] regclass Relation OID used as the catalog key.
 * @param[in] options Borrowed option values copied into the catalog row.
 * @param[in] overwrite Whether an existing row is replaced.
 * @return `true` when the catalog row was inserted or replaced.
 */
bool write_column_options(Oid regclass, column_options* options, bool overwrite) {
    /* Binary-upgrade restores own the catalog rows and must not replace them. */
    Assert(!IsBinaryUpgrade);

    bool written = false;

    NameData compression_name = {0};
    namestrcpy(&compression_name, compression_algorithm_name(options->compression_algorithm));

    options_codec::encoded_row encoded = options_codec::encode(
        options_row{
            .regclass = regclass,
            .chunk_group_row_limit = static_cast<std::int32_t>(options->chunk_group_row_limit),
            .stripe_row_limit = static_cast<std::int32_t>(options->stripe_row_limit),
            .compression_level = static_cast<std::int32_t>(options->compression_level),
            .compression = compression_name,
        },
        CurrentMemoryContext);

    /* create heap tuple and insert into catalog table */
    Relation bioseq_column_options = bioseq::postgres::support::open_postgres_relation(
        column_options_relation_oid(), RowExclusiveLock);
    TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_options);

    /* find existing item to perform update if exist */
    ScanKeyData scan_keys[1] = {0};
    scan_keys[0] = options_codec::scan_key<&options_row::regclass>(regclass);

    Relation index = index_open(column_options_index_oid(), AccessShareLock);
    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_options, index, nullptr, 1, scan_keys);

    HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
    if (HeapTupleIsValid(heap_tuple)) {
        if (overwrite) {
            /* overwrite requests authoritatively replace the stored record */
            options_codec::update update{};
            update
                .set<&options_row::chunk_group_row_limit>(
                    static_cast<std::int32_t>(options->chunk_group_row_limit))
                .set<&options_row::stripe_row_limit>(
                    static_cast<std::int32_t>(options->stripe_row_limit))
                .set<&options_row::compression_level>(
                    static_cast<std::int32_t>(options->compression_level))
                .set<&options_row::compression>(compression_name, CurrentMemoryContext);

            HeapTuple tuple = update.modify(heap_tuple, tuple_descriptor);
            CatalogTupleUpdate(bioseq_column_options, &tuple->t_self, tuple);
            written = true;
        }
    } else {
        /* inserting new record */
        HeapTuple new_tuple = encoded.form_tuple(tuple_descriptor);
        CatalogTupleInsert(bioseq_column_options, new_tuple);
        written = true;
    }

    if (written) {
        CommandCounterIncrement();
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    relation_close(bioseq_column_options, RowExclusiveLock);

    return written;
}

/**
 * @brief Deletes the option row for a columnar relation.
 * @param[in] regclass Relation OID used as the catalog key.
 * @param[in] missing_ok Whether an absent row is accepted.
 * @return `true` when a catalog row was removed.
 * @note Returns `false` when extension removal has made the catalog unavailable.
 */
export bool delete_column_table_options(Oid regclass, bool missing_ok) {
    bool result = false;

    /* Binary-upgrade restores own the catalog rows and must not delete them. */
    Assert(!IsBinaryUpgrade);

    Relation bioseq_column_options = bioseq::postgres::support::try_open_postgres_relation(
        column_options_relation_oid(), RowExclusiveLock);
    if (bioseq_column_options == nullptr) {
        /* extension has been dropped */
        return false;
    }

    /* find existing item to remove */
    ScanKeyData scan_keys[1] = {0};
    scan_keys[0] = options_codec::scan_key<&options_row::regclass>(regclass);

    Relation index = index_open(column_options_index_oid(), AccessShareLock);
    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_options, index, nullptr, 1, scan_keys);

    HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
    if (HeapTupleIsValid(heap_tuple)) {
        CatalogTupleDelete(bioseq_column_options, &heap_tuple->t_self);
        CommandCounterIncrement();

        result = true;
    } else if (!missing_ok) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "missing options for regclass: {}", regclass);
    }

    systable_endscan_ordered(scan_descriptor);
    index_close(index, AccessShareLock);
    relation_close(bioseq_column_options, RowExclusiveLock);

    return result;
}
