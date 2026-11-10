/**
 * @file options.cppm
 * @brief Read and store the size and compression settings for columnar tables.
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <functional>
#include <limits>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "safe_lib.h"
extern "C" {
#include "access/detoast.h"
}

export module bioseq.postgres.storage.metadata:options;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import :types;
import :tuple_ops;
import :core;

/** Visit the existing indexed options row with shared catalog/error cleanup.
 * A missing relation/index yields no result; a missing row is passed as nullptr.
 */
template<class Callback>
[[nodiscard]] inline auto visit_column_options_row(Oid regclass, LOCKMODE mode,
    Callback&& callback) -> std::optional<std::invoke_result_t<Callback, Relation, HeapTuple>> {
    using result_type = std::invoke_result_t<Callback, Relation, HeapTuple>;
    using bioseq::postgres::support::invoke_pg_as_cpp;
    auto* relation = invoke_pg_as_cpp([mode] {
        return bioseq::postgres::support::try_open_postgres_relation(column_options_relation_oid(), mode);
    });
    if (!relation) return std::nullopt;
    Relation index = nullptr;
    SysScanDesc scan = nullptr;
    std::optional<result_type> result;
    std::exception_ptr failure;
    try {
        index = invoke_pg_as_cpp([] {
            return bioseq::postgres::support::try_open_postgres_relation(column_options_index_oid(), AccessShareLock);
        });
        if (index) {
            std::array<ScanKeyData, 1> keys{options_codec::scan_key<&options_row::regclass>(regclass)};
            scan = invoke_pg_as_cpp([relation, index, &keys] {
                return systable_beginscan_ordered(relation, index, nullptr, 1, keys.data());
            });
            auto* tuple = invoke_pg_as_cpp([scan] { return systable_getnext_ordered(scan, ForwardScanDirection); });
            result.emplace(std::invoke(callback, relation, tuple));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    if (scan) record_cleanup_failure(failure, [scan] {
        invoke_pg_as_cpp([scan] { systable_endscan_ordered(scan); });
    });
    if (index) record_cleanup_failure(failure, [index] {
        invoke_pg_as_cpp([index] { index_close(index, AccessShareLock); });
    });
    record_cleanup_failure(failure, [relation, mode] {
        invoke_pg_as_cpp([relation, mode] { relation_close(relation, mode); });
    });
    if (failure) std::rethrow_exception(failure);
    return result;
}

/** Read original file contexts using the options catalog's existing relation key.
 * Returned JSON owns its bytes and is safe after catalog tuples are released.
 */
export [[nodiscard]] std::optional<std::string> read_io_provenance(Oid regclass) {
    const auto found = visit_column_options_row(regclass, AccessShareLock,
        [](Relation relation, HeapTuple tuple) -> std::optional<std::string> {
            if (!HeapTupleIsValid(tuple)) return std::nullopt;
            auto* descriptor = RelationGetDescr(relation);
            options_codec::validate_descriptor(descriptor);
            const auto value = bioseq::postgres::support::invoke_pg_as_cpp([tuple, descriptor] {
                bool is_null = false;
                const Datum datum = heap_getattr(tuple,
                    options_codec::attribute_number<&options_row::io_provenance>(), descriptor, &is_null);
                return is_null ? std::optional<Datum>{} : std::optional<Datum>{datum};
            });
            if (!value) return std::nullopt;
            const auto raw_bytes = bioseq::postgres::support::invoke_pg_as_cpp([value] {
                return toast_raw_datum_size(*value);
            });
            const auto budget = bioseq::common::checked_mul(static_cast<std::uint64_t>(raw_bytes), std::uint64_t{8});
            const auto admitted = budget
                ? bioseq::parallelism::memory_admission_controller::instance().try_reserve(*budget)
                : std::nullopt;
            if (!admitted) bioseq::postgres::support::raise_sql_error(ERRCODE_OUT_OF_MEMORY,
                {.message = "Cannot admit table IO provenance serialization"});
            return bioseq::postgres::support::detail::decode_jsonb(*value).encoded();
        });
    return found ? *found : std::nullopt;
}

/**
 * @brief Read  table options, using current settings if no row exists.
 * @param[in] regclass Table OID.
 * @return Options, or no value if the options table or index is unavailable.
 */
export std::optional<column_options> read_column_options(Oid regclass) {
    return visit_column_options_row(regclass, AccessShareLock,
        [](Relation relation, HeapTuple tuple) -> column_options {
            if (HeapTupleIsValid(tuple)) {
                auto* descriptor = RelationGetDescr(relation);
                options_codec::validate_descriptor(descriptor);
                // Storage settings are read on hot paths. Never deform or copy the
                // potentially large IO manifest when only these five fields matter.
                return bioseq::postgres::support::invoke_pg_as_cpp([tuple, descriptor] {
                    bool is_null = false;
                    const auto get = [tuple, descriptor, &is_null]<auto member>() {
                        const Datum value = heap_getattr(tuple,
                            options_codec::attribute_number<member>(), descriptor, &is_null);
                        if (is_null) ereport(ERROR, (errmsg("Bioseq storage option unexpectedly contains NULL")));
                        return value;
                    };
                    return column_options{
                        .stripe_row_limit = static_cast<uint64>(DatumGetInt32(get.template operator()<&options_row::stripe_row_limit>())),
                        .chunk_group_row_limit = static_cast<uint32>(DatumGetInt32(get.template operator()<&options_row::chunk_group_row_limit>())),
                        .compression_algorithm = parse_compression_algorithm(NameStr(*DatumGetName(get.template operator()<&options_row::compression>()))),
                        .compression_level = DatumGetInt32(get.template operator()<&options_row::compression_level>()),
                    };
                });
            }
            return column_options{
                .stripe_row_limit = static_cast<uint32_t>(bioseq::settings::storage::stripe::row_limit),
                .chunk_group_row_limit = static_cast<uint32_t>(bioseq::settings::storage::chunk_group::row_limit),
                .compression_algorithm = static_cast<bioseq::compression_algorithm>(
                    static_cast<int>(bioseq::settings::storage::compression::algorithm.get_value())),
                .compression_level = bioseq::settings::storage::compression::level,
            };
        });
}

/**
 * @brief Save current, storage settings for a table with no options row.
 * @param[in] regclass Table OID.
 * @note During a binary upgrade, the restored catalog supplies the options.
 */
export void initialize_column_options(Oid regclass) {
    /* A binary upgrade restores the options catalog with the tables. */
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

/** Persist imported IO metadata without replacing storage configuration.
 * Importers validate and rebase their neutral manifest before this catalog update.
 * Existing relation-drop lifecycle deletes this row and its provenance together.
 */
export void write_io_provenance(Oid regclass, std::string_view json) {
    const bioseq::postgres::support::jsonb_value owned{std::string{json}};
    const auto encoded = bioseq::postgres::support::detail::encode_jsonb(owned);
    // The initializer's generated datum arrays borrow PostgreSQL-owned storage.
    // Keep reader, JSON and admission owners outside the PostgreSQL error boundary.
    static_assert(std::is_trivially_destructible_v<options_codec::encoded_row>);
    bioseq::postgres::support::invoke_pg_as_cpp([regclass] {
        initialize_column_options(regclass);
    });
    const auto updated = visit_column_options_row(regclass, RowExclusiveLock,
        [encoded](Relation relation, HeapTuple tuple) {
            if (!HeapTupleIsValid(tuple)) return false;
            options_codec::update change;
            change.set<&options_row::io_provenance>(std::optional<Datum>{encoded}, CurrentMemoryContext);
            auto* modified = change.modify(tuple, RelationGetDescr(relation));
            bioseq::postgres::support::invoke_pg_as_cpp([relation, modified] {
                CatalogTupleUpdate(relation, &modified->t_self, modified);
                heap_freetuple(modified);
                CommandCounterIncrement();
            });
            return true;
        });
    if (!updated || !*updated) bioseq::postgres::support::raise_sql_error(
        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
        {.message = "Bioseq options catalog is unavailable for IO provenance"});
}

/// @brief Save new columnar options for a table, replacing any existing row.
/// @param[in] regclass Table OID.
/// @param[in] options Values to copy into  the catalog.
export void set_column_options(Oid regclass, column_options* options) {
    write_column_options(regclass, options, true);
}

/**
 * @brief Insert or update a table's columnar options.
 * @param[in] regclass Table OID.
 * @param[in] options Values to copy into the catalog.
 * @param[in] overwrite Allow an existing row to be replaced.
 * @return true if a row was inserted or updated.
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

    /* Open the options catalog for writing */
    Relation bioseq_column_options = bioseq::postgres::support::open_postgres_relation(
        column_options_relation_oid(), RowExclusiveLock);
    TupleDesc tuple_descriptor = RelationGetDescr(bioseq_column_options);

    /* Find the table's existing options. */
    ScanKeyData scan_keys[1] = {0};
    scan_keys[0] = options_codec::scan_key<&options_row::regclass>(regclass);

    Relation index = index_open(column_options_index_oid(), AccessShareLock);
    SysScanDesc scan_descriptor =
        systable_beginscan_ordered(bioseq_column_options, index, nullptr, 1, scan_keys);

    HeapTuple heap_tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
    if (HeapTupleIsValid(heap_tuple)) {
        if (overwrite) {
            /* Replace all stored option values. */
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
        /* Insert options for a table with no catalog row. */
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
 * @brief Remove a table's coluznar options.
 * @param[in] regclass Table OID.
 * @param[in] missing_ok Allow a missing options row. Otherwise raise an error.
 * @return true if a row was deleted, or false if the row or catalog is absent.
 */
export bool delete_column_table_options(Oid regclass, bool missing_ok) {
    bool result = false;

    /* Binary-upgrade restores own the catalog rows and must not delete them. */
    Assert(!IsBinaryUpgrade);

    Relation bioseq_column_options = bioseq::postgres::support::try_open_postgres_relation(
        column_options_relation_oid(), RowExclusiveLock);
    if (bioseq_column_options == nullptr) {
        /* The catalog is already gone. */
        return false;
    }

    /* Find the table's options row. */
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
