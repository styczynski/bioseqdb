/**
 * @file toast_faults.cppm
 * @brief Constructs external-TOAST fixtures and verifies PostgreSQL fault unwinding.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "miscadmin.h"
#include "varatt.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.support.row_codec_runtime_test_support:toast_faults;

import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.indexing.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import :contract_core;
import :catalog_lifecycle;

namespace bioseq::postgres::support::row_codec_runtime_test_support {

[[nodiscard]] std::string make_incompressible_text(std::size_t size, std::uint64_t seed) {
    static constexpr std::string_view k_alphabet{
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz!@#$%^&*()-_=+[]{}"};
    std::string result;
    result.resize(size);
    std::uint64_t state = seed;
    for (char& value : result) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        value = k_alphabet[
            static_cast<std::size_t>(state % k_alphabet.size())];
    }
    return result;
}

[[nodiscard]] text* make_external_text(MemoryContext context, std::uint64_t seed) {
    const std::string value = make_incompressible_text(65536, seed);
    return make_text(value, context);
}

[[nodiscard]] bytea* make_external_bytea(MemoryContext context, std::uint64_t seed) {
    const std::string value = make_incompressible_text(65536, seed);
    return make_bytea(value, context);
}

[[nodiscard]] std::string make_external_string(std::uint64_t seed) {
    return make_incompressible_text(65536, seed);
}

template <class p_codec, class p_exercise>
[[nodiscard]] runtime_receipt_entry
with_column_mapping_toast_carrier(TupleDesc installed_descriptor, p_exercise exercise) {
    validate_descriptor_mutations<p_codec>(installed_descriptor);
    invoke_pg_as_cpp([] {
        const int connected = SPI_connect();
        if (connected != SPI_OK_CONNECT) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal("could not connect SPI for "
                                     "column-mapping TOAST carrier")));
        }
        const int created = SPI_execute("CREATE TEMP TABLE "
                                        "column_mapping_codec_carrier ("
                                        "column_name text, data_source text"
                                        ") ON COMMIT DROP",
                                        false,
                                        0);
        const int finished = SPI_finish();
        if (created != SPI_OK_UTILITY || finished != SPI_OK_FINISH) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal("could not create column-mapping "
                                     "TOAST carrier")));
        }
    });
    const Oid relation_oid = invoke_pg_as_cpp([] {
        Oid temp_namespace = InvalidOid;
        Oid temp_toast_namespace = InvalidOid;
        GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
        const Oid resolved = get_relname_relid("column_mapping_codec_carrier", temp_namespace);
        if (!OidIsValid(resolved)) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg_internal("column-mapping TOAST carrier "
                                     "is missing")));
        }
        return resolved;
    });
    Relation relation =
        invoke_pg_as_cpp([relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
    const runtime_receipt_entry result = exercise(RelationGetDescr(relation), relation);
    invoke_pg_as_cpp([relation] { table_close(relation, RowExclusiveLock); });
    return result;
}

[[nodiscard]] Oid find_unused_reaper_database_oid(Relation relation, TupleDesc descriptor) {
    using row = manager_rows::minimizer_generation_reaper_state;
    using codec = manager_codecs::minimizer_generation_reaper_state_codec;
    Oid candidate = std::numeric_limits<Oid>::max() - 1024U;
    while (candidate != InvalidOid) {
        ScanKeyData key = codec::scan_key<&row::database_oid>(candidate);
        SysScanDesc scan = invoke_pg_as_cpp([relation, &key] {
            return systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 1, &key);
        });
        HeapTuple tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
        const bool absent = !HeapTupleIsValid(tuple);
        invoke_pg_as_cpp([scan] { systable_endscan(scan); });
        if (absent) {
            return candidate;
        }
        --candidate;
    }
    static_cast<void>(descriptor);
    contract_failure("could not reserve a reaper-state probe identity");
}

void exercise_reaper_external_toast() {
    using row = manager_rows::minimizer_generation_reaper_state;
    using codec = manager_codecs::minimizer_generation_reaper_state_codec;

    const Oid namespace_oid =
        invoke_pg_as_cpp([] { return get_namespace_oid("bioseq_internal", false); });
    const Oid relation_oid = invoke_pg_as_cpp([namespace_oid] {
        return get_relname_relid("minimizer_generation_reaper_state", namespace_oid);
    });
    if (!OidIsValid(relation_oid)) {
        contract_failure("reaper-state relation is missing");
    }
    Relation relation =
        invoke_pg_as_cpp([relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
    TupleDesc descriptor = RelationGetDescr(relation);
    codec::validate_descriptor(descriptor);
    if (!OidIsValid(relation->rd_rel->reltoastrelid)) {
        contract_failure("reaper-state relation has no TOAST relation");
    }

    const Oid database_oid = find_unused_reaper_database_oid(relation, descriptor);
    const std::string initial = make_incompressible_text(131072, 0x388A11U);
    const std::string updated = make_incompressible_text(196608, 0x388B22U);
    MemoryContext value_context = make_context();
    MemoryContext result_context = make_context();

    {
        const row value{
            .database_oid = database_oid,
            .tablespace_root_ordinal = 7,
            .shard_ordinal = 17,
            .last_canonical_filename = initial,
            .pass = 388,
        };
        auto encoded = codec::encode(value, value_context);
        HeapTuple tuple = encoded.form_tuple(descriptor);
        invoke_pg_as_cpp([relation, tuple] { CatalogTupleInsert(relation, tuple); });
        invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
        invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    }
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });

    ScanKeyData key = codec::scan_key<&row::database_oid>(database_oid);
    SysScanDesc scan = invoke_pg_as_cpp([relation, &key] {
        return systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 1, &key);
    });
    HeapTuple tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
    if (!HeapTupleIsValid(tuple)) {
        contract_failure("external-toast reaper row was not found");
    }
    bool is_null = false;
    const Datum raw_text = invoke_pg_as_cpp(
        [tuple, descriptor, &is_null] { return heap_getattr(tuple, 4, descriptor, &is_null); });
    if (is_null || !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw_text))) {
        contract_failure("reaper filename was not stored out of line");
    }
    const row decoded = codec::deform(tuple, descriptor, result_context);
    codec::update changes{};
    changes.set<&row::last_canonical_filename>(updated, value_context);
    changes.set<&row::pass>(static_cast<std::int64_t>(776));
    HeapTuple changed = changes.modify(tuple, descriptor);
    invoke_pg_as_cpp(
        [relation, tuple, changed] { CatalogTupleUpdate(relation, &tuple->t_self, changed); });
    invoke_pg_as_cpp([changed] { heap_freetuple(changed); });
    invoke_pg_as_cpp([scan] { systable_endscan(scan); });
    invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    if (decoded.last_canonical_filename != initial || decoded.pass != 388) {
        contract_failure("external-toast decode did not survive scan release");
    }
    reset_context_once(result_context,
                       "bioseq_internal.minimizer_generation_reaper_state"
                       " external-toast result");

    const int connected = invoke_pg_as_cpp([] { return SPI_connect(); });
    if (connected != SPI_OK_CONNECT) {
        contract_failure("could not connect SPI for external-toast probe");
    }
    const std::string query =
        fmt::format("SELECT database_oid, tablespace_root_ordinal, "
                    "       shard_ordinal, last_canonical_filename, pass "
                    "  FROM bioseq_internal.minimizer_generation_reaper_state "
                    " WHERE database_oid = {}::oid",
                    database_oid);
    const int status = invoke_pg_as_cpp([&query] { return SPI_execute(query.c_str(), true, 1); });
    if (status != SPI_OK_SELECT || SPI_processed != 1 || SPI_tuptable == nullptr) {
        contract_failure("SPI did not return the updated reaper row");
    }
    HeapTuple spi_tuple = SPI_tuptable->vals[0];
    TupleDesc spi_descriptor = SPI_tuptable->tupdesc;
    codec::validate_descriptor(spi_descriptor);
    is_null = false;
    const Datum updated_raw_text = invoke_pg_as_cpp([spi_tuple, spi_descriptor, &is_null] {
        return heap_getattr(spi_tuple, 4, spi_descriptor, &is_null);
    });
    if (is_null || !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(updated_raw_text))) {
        contract_failure("updated reaper filename was not external TOAST");
    }
    const row updated_decoded = codec::deform(spi_tuple, spi_descriptor, result_context);
    const int finished = invoke_pg_as_cpp([] { return SPI_finish(); });
    if (finished != SPI_OK_FINISH) {
        contract_failure("could not release SPI tuple table");
    }
    if (updated_decoded.last_canonical_filename != updated || updated_decoded.pass != 776) {
        contract_failure("external-toast update/deform did not survive SPI release");
    }
    reset_context_once(result_context,
                       "bioseq_internal.minimizer_generation_reaper_state "
                       "SPI result");

    invoke_pg_as_cpp([result_context] { MemoryContextDelete(result_context); });
    invoke_pg_as_cpp([value_context] { MemoryContextDelete(value_context); });
    invoke_pg_as_cpp([relation] { table_close(relation, RowExclusiveLock); });
}

[[nodiscard]] Oid find_nonexistent_relation_oid() {
    Oid candidate = std::numeric_limits<Oid>::max() - 4096U;
    while (candidate != InvalidOid) {
        const bool exists = invoke_pg_as_cpp(
            [candidate] { return SearchSysCacheExists1(RELOID, ObjectIdGetDatum(candidate)); });
        if (!exists) {
            return candidate;
        }
        --candidate;
    }
    contract_failure("could not find a nonexistent relation OID");
}

struct unwind_sentinel final {
    std::size_t* destructions;

    ~unwind_sentinel() noexcept {
        ++*destructions;
    }
};

void exercise_reaper_text_bridge_error_impl() {
    using row = manager_rows::minimizer_generation_reaper_state;
    using codec = manager_codecs::minimizer_generation_reaper_state_codec;
    const Oid relation_type_oid = invoke_pg_as_cpp([] {
        const Oid namespace_oid = get_namespace_oid("bioseq_internal", false);
        const Oid relation_oid =
            get_relname_relid("minimizer_generation_reaper_state", namespace_oid);
        return get_rel_type_id(relation_oid);
    });
    TupleDesc descriptor = invoke_pg_as_cpp(
        [relation_type_oid] { return lookup_rowtype_tupdesc(relation_type_oid, -1); });
    MemoryContext value_context = make_context();
    MemoryContext result_context = make_context();

    const row value{
        .database_oid = MyDatabaseId,
        .tablespace_root_ordinal = 0,
        .shard_ordinal = 0,
        .last_canonical_filename = "bridge-error-source",
        .pass = 0,
    };
    auto encoded = codec::encode(value, value_context);
    auto data = encoded.tuple_data(descriptor);

    alignas(std::max_align_t) std::array<unsigned char, TOAST_POINTER_SIZE> external_bytes{};
    auto* external = reinterpret_cast<varattrib_1b_e*>(external_bytes.data());
    SET_VARTAG_EXTERNAL(external, VARTAG_ONDISK);
    const varatt_external toast_pointer{
        .va_rawsize = VARHDRSZ + 4,
        .va_extinfo = 4,
        .va_valueid = 1,
        .va_toastrelid = find_nonexistent_relation_oid(),
    };
    std::memcpy(VARDATA_EXTERNAL(external), &toast_pointer, sizeof(toast_pointer));
    if (!VARATT_IS_EXTERNAL_ONDISK(external) || VARSIZE_EXTERNAL(external) != TOAST_POINTER_SIZE) {
        contract_failure("fault fixture did not construct a valid "
                         "on-disk TOAST pointer");
    }
    data.values[3] = PointerGetDatum(external);
    data.nulls[3] = false;
    HeapTuple tuple = encoded.form_tuple(descriptor);

    std::size_t destructor_count = 0;
    try {
        unwind_sentinel sentinel{
            .destructions = &destructor_count,
        };
        static_cast<void>(codec::deform(tuple, descriptor, result_context));
    } catch (postgres_error& error) {
        if (destructor_count != 1) {
            contract_failure("reaper text leaf did not unwind exactly once");
        }
        if (error.get_sql_state_code() != ERRCODE_INTERNAL_ERROR ||
            std::string_view{error.what()}.find("could not open relation with OID") ==
                std::string_view::npos) {
            contract_failure("reaper text leaf did not preserve the "
                             "PostgreSQL ERROR");
        }
        throw;
    }
    contract_failure("invalid external TOAST pointer did not raise");
}

} // namespace bioseq::postgres::support::row_codec_runtime_test_support
